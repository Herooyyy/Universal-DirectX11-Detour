// Standard imports
#include <Windows.h>
#include <iostream>

// Detours imports
#include "detours.h"

// DX11 imports
#include <d3d11.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define safe_release(p) if (p) { p->Release(); p = nullptr; } 
// D3X HOOK DEFINITIONS
typedef HRESULT(__fastcall *IDXGISwapChainPresent)(IDXGISwapChain *pSwapChain, UINT SyncInterval, UINT Flags);

// Main D3D11 Objects
ID3D11DeviceContext *pContext = NULL;
ID3D11Device *pDevice = NULL;
ID3D11RenderTargetView *mainRenderTargetView;
static IDXGISwapChain*  pSwapChain = NULL;
static WNDPROC OriginalWndProcHandler = nullptr;
HWND window = nullptr;
IDXGISwapChainPresent fnIDXGISwapChainPresent;
DWORD_PTR* pDeviceContextVTable = NULL;
ID3D11VertexShader* pVertexShader = nullptr;
ID3D11InputLayout* pVertexLayout = nullptr;
ID3D11PixelShader* pPixelShader = nullptr;
ID3D11Buffer* pVertexBuffer = nullptr;
ID3D11Buffer* pIndexBuffer = nullptr;
ID3D11Buffer* pConstantBuffer = nullptr;
struct ConstantBuffer
{
	DirectX::XMMATRIX mProjection;
};

struct Vertex
{
	DirectX::XMFLOAT3 pos;
	DirectX::XMFLOAT4 color;
};
DirectX::XMMATRIX mOrtho;
HWND hWnd;

#define MAINVP 0
D3D11_VIEWPORT pViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{ 0 };

// Boolean
BOOL g_bInitialised = false;
bool g_PresentHooked = false;

void ConsoleSetup()
{
	AllocConsole();
	SetConsoleTitle("DirectX11 Detour");
	freopen("CONOUT$", "w", stdout);
	freopen("CONOUT$", "w", stderr);
	freopen("CONIN$", "r", stdin);
}

LRESULT CALLBACK hWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	POINT mPos;
	GetCursorPos(&mPos);
	ScreenToClient(window, &mPos);

	return CallWindowProc(OriginalWndProcHandler, hWnd, uMsg, wParam, lParam);
}

HRESULT GetDeviceAndCtxFromSwapchain(IDXGISwapChain *pSwapChain, ID3D11Device **ppDevice, ID3D11DeviceContext **ppContext)
{
	HRESULT ret = pSwapChain->GetDevice(__uuidof(ID3D11Device), (PVOID*)ppDevice);

	if (SUCCEEDED(ret))
		(*ppDevice)->GetImmediateContext(ppContext);

	return ret;
}

bool CompileShader(const char* szShader, const char* szEntrypoint, const char* szTarget, ID3D10Blob** pBlob)
{
	ID3D10Blob* pErrorBlob = nullptr;

	auto hr = D3DCompile(szShader, strlen(szShader), 0, nullptr, nullptr, szEntrypoint, szTarget, D3DCOMPILE_ENABLE_STRICTNESS, 0, pBlob, &pErrorBlob);
	if (FAILED(hr))
	{
		if (pErrorBlob)
		{
			char szError[256]{ 0 };
			memcpy(szError, pErrorBlob->GetBufferPointer(), pErrorBlob->GetBufferSize());
			MessageBoxA(nullptr, szError, "Error", MB_OK);
		}
		return false;
	}
	return true;
}

void Render()
{
	pContext->OMSetRenderTargets(1, &mainRenderTargetView, nullptr);

	ConstantBuffer cb;
	cb.mProjection = XMMatrixTranspose(mOrtho);
	pContext->UpdateSubresource(pConstantBuffer, 0, nullptr, &cb, 0, 0);
	pContext->VSSetConstantBuffers(0, 1, &pConstantBuffer);

	UINT stride = sizeof(Vertex);
	UINT offset = 0;
	pContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &stride, &offset);
	pContext->IASetInputLayout(pVertexLayout);
	pContext->IASetIndexBuffer(pIndexBuffer, DXGI_FORMAT_R32_UINT, 0);
	pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	pContext->VSSetShader(pVertexShader, nullptr, 0);
	pContext->PSSetShader(pPixelShader, nullptr, 0);

	pContext->RSSetViewports(1, pViewports);

	pContext->DrawIndexed(3, 0, 0);
}

HRESULT __fastcall Present(IDXGISwapChain *pChain, UINT SyncInterval, UINT Flags)
{
	if (!g_bInitialised) {
		g_PresentHooked = true;
		std::cout << "\t[+] Present Hook called by first time" << std::endl;
		if (FAILED(GetDeviceAndCtxFromSwapchain(pChain, &pDevice, &pContext)))
			return fnIDXGISwapChainPresent(pChain, SyncInterval, Flags);
		pSwapChain = pChain;
		DXGI_SWAP_CHAIN_DESC sd;
		pChain->GetDesc(&sd);
		window = sd.OutputWindow;

		OriginalWndProcHandler = (WNDPROC)SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)hWndProc);

		ID3D11Texture2D* pBackBuffer;

		pChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
		pDevice->CreateRenderTargetView(pBackBuffer, NULL, &mainRenderTargetView);
		pBackBuffer->Release();

		g_bInitialised = true;

		pContext->OMSetRenderTargets(1, &mainRenderTargetView, NULL);

		ID3D10Blob* pBlob = nullptr;

		constexpr const char* szShadez = R"(
		// Constant buffer
		cbuffer ConstantBuffer : register(b0)
		{
			matrix projection;
		}

		// PSI (PixelShaderInput)
		struct PSI
		{
			float4 pos : SV_POSITION;
			float4 color : COLOR;
		};

		// VertexShader
		PSI VS( float4 pos : POSITION, float4 color : COLOR )
		{
			PSI psi;
			psi.color = color;
			pos = mul( pos, projection );
			psi.pos = pos;
			return psi;
		}

		// PixelShader
		float4 PS(PSI psi) : SV_TARGET
		{
			return psi.color;
		}
		)";
		if (!CompileShader(szShadez, "VS", "vs_5_0", &pBlob))
		{
			MessageBox(NULL, "Failed to compile vertex shader", "error", NULL);
			return false;
		}
		std::cout << "[+] Compiled Vertex Shader" << std::endl;

		HRESULT hr = pDevice->CreateVertexShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &pVertexShader);
		if (FAILED(hr))
		{
			MessageBox(NULL, "Create vertex shader error", "error", NULL);
			return false;
		}
		std::cout << "[+] Created Vertex Shader Buffer" << std::endl;

		D3D11_INPUT_ELEMENT_DESC layout[2] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0}
		};
		UINT numElements = ARRAYSIZE(layout);

		hr = pDevice->CreateInputLayout(layout, numElements, pBlob->GetBufferPointer(), pBlob->GetBufferSize(), &pVertexLayout);
		if (FAILED(hr))
		{
			MessageBox(NULL, "Create Input layout error", "error", NULL);
			return false;
		}
		std::cout << "[+] Created Input layour for Vertex Shader" << std::endl;

		safe_release(pBlob);

		if (!CompileShader(szShadez, "PS", "ps_5_0", &pBlob))
		{
			MessageBox(NULL, "Failed to compile ps", "error", NULL);
			return false;
		}
		std::cout << "[+] Compiled Pixel Shader" << std::endl;

		hr = pDevice->CreatePixelShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &pPixelShader);
		if (FAILED(hr))
		{
			MessageBox(NULL, "Create pixel shader error", "error", NULL);
			return false;
		}
		std::cout << "[+] Created Pixel Shader Buffer" << std::endl;

		UINT numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		float fWidth = 0;
		float fHeight = 0;

		pContext->RSGetViewports(&numViewports, pViewports);

		if (!numViewports || !pViewports[MAINVP].Width)
		{
			RECT rc{ 0 };
			if (!GetClientRect(hWnd, &rc))
			{
				MessageBox(NULL, "Get client rect", "error", NULL);
				return false;
			}

			fWidth = (float)rc.right;
			fHeight = (float)rc.bottom;

			pViewports[MAINVP].Width = (float)fWidth;
			pViewports[MAINVP].Height = (float)fHeight;
			pViewports[MAINVP].MinDepth = 0.0f;
			pViewports[MAINVP].MaxDepth = 1.0f;

			pContext->RSSetViewports(1, pViewports);
		}
		else
		{
			fWidth = (float)pViewports[MAINVP].Width;
			fHeight = (float)pViewports[MAINVP].Height;
		}
		D3D11_BUFFER_DESC bd{ 0 };
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.ByteWidth = sizeof(ConstantBuffer);
		bd.Usage = D3D11_USAGE_DEFAULT;

		mOrtho = DirectX::XMMatrixOrthographicLH(fWidth, fHeight, 0.0f, 1.0f);
		ConstantBuffer cb;
		cb.mProjection = mOrtho;

		D3D11_SUBRESOURCE_DATA sr{ 0 };
		sr.pSysMem = &cb;
		hr = pDevice->CreateBuffer(&bd, &sr, &pConstantBuffer);
		std::cout << "[+] Created constant buffer for shader" << std::endl;
		if (FAILED(hr))
		{
			MessageBox(NULL, "failed create buffer 1", "error", NULL);
			return false;
		}

		ZeroMemory(&bd, sizeof(bd));
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.ByteWidth = 3 * sizeof(Vertex);
		bd.StructureByteStride = sizeof(Vertex);

		float left = fWidth / -2;
		float top = fHeight / 2;
		float w = 50;
		float h = 50;

		float fPosX = -1 * left;
		float fPosY = top;

		Vertex pVerts[3] = {
			{ DirectX::XMFLOAT3(left + fPosX,			top - fPosY + h / 2,	1.0f),	DirectX::XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f) },
			{ DirectX::XMFLOAT3(left + fPosX + w / 2,	top - fPosY - h / 2,	1.0f),	DirectX::XMFLOAT4(0.0f, 0.0f, 1.0f, 1.0f) },
			{ DirectX::XMFLOAT3(left + fPosX - w / 2,	top - fPosY - h / 2,	1.0f),	DirectX::XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f) },
		};
		std::cout << "[+] Created triangle vertices" << std::endl;

		ZeroMemory(&sr, sizeof(sr));
		sr.pSysMem = &pVerts;
		hr = pDevice->CreateBuffer(&bd, &sr, &pVertexBuffer);
		if (FAILED(hr))
		{
			MessageBox(NULL, "failed create buffer 2", "error", NULL);
			return false;
		}
		std::cout << "[+] Created buffer for vertices" << std::endl;

		ZeroMemory(&bd, sizeof(bd));
		ZeroMemory(&sr, sizeof(sr));

		UINT pIndices[3] = { 0, 1, 2 };
		bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.ByteWidth = sizeof(UINT) * 3;
		bd.StructureByteStride = sizeof(UINT);

		sr.pSysMem = &pIndices;
		hr = pDevice->CreateBuffer(&bd, &sr, &pIndexBuffer);
		if (FAILED(hr))
		{
			MessageBox(NULL, "create buffer 3", "error", NULL);
			return false;
		}
		std::cout << "[+] Created buffer for indices" << std::endl;

		return true;
	}

	Render();

	return fnIDXGISwapChainPresent(pChain, SyncInterval, Flags);
}

void detourDirectXPresent()
{
	std::cout << "[+] Calling fnIDXGISwapChainPresent Detour" << std::endl;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(LPVOID&)fnIDXGISwapChainPresent, (PBYTE)Present);
	DetourTransactionCommit();
}

void printValues()
{
	std::cout << "[+] ID3D11DeviceContext Addr: " << std::hex << pContext << std::endl;
	std::cout << "[+] ID3D11Device Addr: " << std::hex << pDevice << std::endl;
	std::cout << "[+] ID3D11RenderTargetView Addr: " << std::hex << mainRenderTargetView << std::endl;
	std::cout << "[+] IDXGISwapChain Addr: " << std::hex << pSwapChain << std::endl;
}

void GetPresent()
{
	hWnd = FindWindowA(NULL, "DARK SOULS III");

	DXGI_SWAP_CHAIN_DESC sd;
	ZeroMemory(&sd, sizeof(sd));
	sd.BufferCount = 1;
	sd.BufferDesc.Width = 2;
	sd.BufferDesc.Height = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hWnd;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.Windowed = TRUE;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	D3D_FEATURE_LEVEL FeatureLevelsRequested = D3D_FEATURE_LEVEL_11_0;
	UINT numFeatureLevelsRequested = 1;
	D3D_FEATURE_LEVEL FeatureLevelsSupported;
	HRESULT hr;
	IDXGISwapChain *swapchain = 0;
	ID3D11Device *dev = 0;
	ID3D11DeviceContext *devcon = 0;
	if (FAILED(hr = D3D11CreateDeviceAndSwapChain(NULL,
		D3D_DRIVER_TYPE_HARDWARE,
		NULL,
		0,
		&FeatureLevelsRequested,
		numFeatureLevelsRequested,
		D3D11_SDK_VERSION,
		&sd,
		&swapchain,
		&dev,
		&FeatureLevelsSupported,
		&devcon)))
	{
		std::cout << "[-] Failed to hook Present with VT method." << std::endl;
		return;		
	}
	DWORD_PTR* pSwapChainVtable = NULL;
	pSwapChainVtable = (DWORD_PTR*)swapchain;
	pSwapChainVtable = (DWORD_PTR*)pSwapChainVtable[0];
	fnIDXGISwapChainPresent = (IDXGISwapChainPresent)(DWORD_PTR)pSwapChainVtable[8];
	g_PresentHooked = true;

	MessageBox(NULL, "PRESENT", "PRESENT", MB_OK);
	std::cout << "[+] Present Addr:" << fnIDXGISwapChainPresent << std::endl;
	Sleep(2000);
}

int WINAPI main(void* pHandle)
{
	ConsoleSetup();

	GetPresent();

	detourDirectXPresent();
	while (!g_bInitialised) {
		Sleep(1000);
	}
	printValues();

	std::cout << "[+] pDeviceContextVTable0 Addr: " << std::hex << pContext << std::endl;
	pDeviceContextVTable = (DWORD_PTR*)pContext;
	std::cout << "[+] pDeviceContextVTable1 Addr: " << std::hex << pDeviceContextVTable << std::endl;
	pDeviceContextVTable = (DWORD_PTR*)pDeviceContextVTable[0];
	std::cout << "[+] pDeviceContextVTable2 Addr: " << std::hex << pDeviceContextVTable << std::endl;

	Sleep(4000);
	
}

BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		DisableThreadLibraryCalls(hModule);
		CreateThread(NULL, NULL, (LPTHREAD_START_ROUTINE)main, NULL, NULL, NULL);
	}
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

