#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include "GakuPlugin.h"

using Microsoft::WRL::ComPtr;

void BridgeLog(const char* fmt, ...);
using PresentCall = HRESULT (*)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
bool FgPresent(IDXGISwapChain* sc, UINT sync, UINT flags, PresentCall original,
	const DXGI_PRESENT_PARAMETERS* parameters, HRESULT* hr);

namespace
{
	typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
	typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
	using CreateFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
	using CreateForHwndFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
		const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

	PresentFn g_present = nullptr;
	Present1Fn g_present1 = nullptr;
	CreateFn g_create = nullptr;
	CreateForHwndFn g_createForHwnd = nullptr;
	std::atomic<bool> g_capture{ false };
	bool g_tried = false;
	std::atomic<bool> g_hooked{ false };
	std::atomic<HWND> g_window{ nullptr };
	std::atomic<uint64_t> g_presents{ 0 };
	thread_local bool g_inHook = false;

	const char* EffectName(DXGI_SWAP_EFFECT e)
	{
		switch (e)
		{
		case DXGI_SWAP_EFFECT_DISCARD: return "DISCARD (bitblt)";
		case DXGI_SWAP_EFFECT_SEQUENTIAL: return "SEQUENTIAL (bitblt)";
		case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "FLIP_SEQUENTIAL";
		case DXGI_SWAP_EFFECT_FLIP_DISCARD: return "FLIP_DISCARD";
		default: return "?";
		}
	}

	HRESULT PresentWrapped(IDXGISwapChain* sc, UINT sync, UINT flags, PresentCall original,
		const DXGI_PRESENT_PARAMETERS* parameters)
	{
		HRESULT hr;
		bool ours = false;
		if (!g_inHook)
		{
			g_inHook = true;
			ours = FgPresent(sc, sync, flags, original, parameters, &hr);
			g_presents++;
			if (!ours) hr = original(sc, sync, flags, parameters);
			g_inHook = false;
			return hr;
		}
		return original(sc, sync, flags, parameters);
	}

	HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* sc, UINT sync, UINT flags)
	{
		return PresentWrapped(sc, sync, flags, [](IDXGISwapChain* chain, UINT interval, UINT options,
			const DXGI_PRESENT_PARAMETERS*) { return g_present(chain, interval, options); }, nullptr);
	}

	HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
	{
		return PresentWrapped(sc, sync, flags, [](IDXGISwapChain* chain, UINT interval, UINT options,
			const DXGI_PRESENT_PARAMETERS* params) {
			return g_present1(static_cast<IDXGISwapChain1*>(chain), interval, options, params);
		}, p);
	}

	bool Patch(void** slot, void* hook, void** original)
	{
		DWORD was = 0;
		if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &was)) { BridgeLog("Present hook: VirtualProtect failed (error %lu).", GetLastError()); return false; }
		*original = *slot;
		InterlockedExchangePointer(slot, hook);
		VirtualProtect(slot, sizeof(void*), was, &was);
		return true;
	}

	void HookChain(IDXGISwapChain* chain)
	{
		// The actual Unity chain and a temporary chain can have different vtables.
		if (g_hooked) return;
		auto vtable = *reinterpret_cast<void***>(chain);
		const bool present = Patch(&vtable[8], reinterpret_cast<void*>(&PresentHook), reinterpret_cast<void**>(&g_present));
		ComPtr<IDXGISwapChain1> chain1;
		bool present1 = false;
		if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1))))
			present1 = Patch(&(*reinterpret_cast<void***>(chain1.Get()))[22],
				reinterpret_cast<void*>(&Present1Hook), reinterpret_cast<void**>(&g_present1));
		g_hooked = present || present1;
		BridgeLog("Present hook: Present=%d Present1=%d.", present, present1);
	}

	void CaptureChain(HWND window, IDXGISwapChain* chain)
	{
		wchar_t name[64]{};
		if (GetClassNameW(window, name, _countof(name)) && wcscmp(name, L"UnityWndClass") == 0) {
			g_window = window;
			HookChain(chain);
		}
	}

	HRESULT STDMETHODCALLTYPE CreateHook(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
		IDXGISwapChain** chain)
	{
		const HRESULT hr = g_create(factory, device, desc, chain);
		if (SUCCEEDED(hr) && desc && chain && *chain) CaptureChain(desc->OutputWindow, *chain);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE CreateForHwndHook(IDXGIFactory2* factory, IUnknown* device, HWND window,
		const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
		IDXGIOutput* output, IDXGISwapChain1** chain)
	{
		const HRESULT hr = g_createForHwnd(factory, device, window, desc, fullscreen, output, chain);
		if (SUCCEEDED(hr) && chain && *chain) CaptureChain(window, *chain);
		return hr;
	}

	void Hook(ID3D11Resource* any)
	{
		if (g_tried) return;
		g_tried = true;
		ComPtr<ID3D11Device> dev;
		any->GetDevice(&dev);
		ComPtr<IDXGIDevice> xd;
		ComPtr<IDXGIAdapter> ad;
		ComPtr<IDXGIFactory2> factory;
		if (FAILED(dev.As(&xd)) || FAILED(xd->GetAdapter(&ad)) || FAILED(ad->GetParent(IID_PPV_ARGS(&factory)))) { BridgeLog("Present hook: no IDXGIFactory2 from the game's device."); return; }
		ComPtr<IDXGIFactory5> f5;
		BOOL tearing = FALSE;
		if (SUCCEEDED(factory.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
		WNDCLASSW wc = {};
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"FSR4PresentHook";
		RegisterClassW(&wc);
		HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"FSR4 present hook", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
		if (!hwnd) { BridgeLog("Present hook: CreateWindowExW failed (error %lu).", GetLastError()); return; }
		DXGI_SWAP_CHAIN_DESC1 d = {};
		d.Width = 64; d.Height = 64;
		d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		d.SampleDesc.Count = 1;
		d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		d.BufferCount = 2;
		d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		ComPtr<IDXGISwapChain1> dummy;
		HRESULT hr = factory->CreateSwapChainForHwnd(dev.Get(), hwnd, &d, nullptr, nullptr, &dummy);
		if (FAILED(hr))
		{
			d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
			d.BufferCount = 1;
			hr = factory->CreateSwapChainForHwnd(dev.Get(), hwnd, &d, nullptr, nullptr, &dummy);
		}
		if (FAILED(hr)) { BridgeLog("Present hook: dummy swap chain failed: 0x%08X.", (unsigned)hr); DestroyWindow(hwnd); return; }
		HookChain(dummy.Get());
		BridgeLog("Present hook: temporary chain %s, tearing support %d.", EffectName(d.SwapEffect), tearing);
		dummy.Reset();
		DestroyWindow(hwnd);
	}
}

bool CaptureGameSwapChain(const GakuHookService& hooks)
{
	ComPtr<IDXGIFactory2> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
	auto vtable = *reinterpret_cast<void***>(factory.Get());
	GakuHookRequest requests[]{
		{vtable[10], reinterpret_cast<void*>(&CreateHook), reinterpret_cast<void**>(&g_create)},
		{vtable[15], reinterpret_cast<void*>(&CreateForHwndHook), reinterpret_cast<void**>(&g_createForHwnd)}
	};
	g_capture = hooks.acquireBatch(hooks.context, _countof(requests), requests) == GAKU_OK;
	return g_capture;
}

void InstallPresentHook(ID3D11Resource* anchor)
{
	if (!g_capture) Hook(anchor);
}

bool IsPresentWindow(HWND window) { return !g_capture || window == g_window.load(); }
bool PresentHooked() { return g_hooked; }
uint64_t PresentsSeen() { return g_presents.load(); }
