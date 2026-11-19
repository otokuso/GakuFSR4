#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include "GakuPlugin.h"
#include "../../src/PresentHookSlots.h"
#include "../../src/PresentHookRetry.h"
#include "../../src/PresentRate.h"
#include "../../src/LiveTiming.h"
#include "../../src/NativeSwapchain.h"

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

	std::mutex g_slotsLock;
	PresentHookSlots g_slots;
	CreateFn g_create = nullptr;
	CreateForHwndFn g_createForHwnd = nullptr;
	std::atomic<bool> g_capture{ false };
	std::atomic<bool> g_tryNative{ false };
	PresentHookRetry g_probeRetry;
	std::atomic<bool> g_hooked{ false };
	std::atomic<HWND> g_window{ nullptr };
	std::atomic<uint64_t> g_presents{ 0 };
	std::atomic<uint64_t> g_realFrames{ 0 }, g_generatedFrames{ 0 };
	std::mutex g_timingLock;
	LiveTimingTracker g_liveTiming;
	std::mutex g_inputHookLock;
	HHOOK g_inputHook = nullptr;
	std::atomic<HWND> g_inputWindow{ nullptr };
	uint64_t g_nextInputHookAttempt = 0;
	thread_local bool g_inHook = false;

	bool IsInputMessage(UINT message)
	{
		switch (message) {
		case WM_INPUT:
		case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
		case WM_RBUTTONDOWN: case WM_RBUTTONUP:
		case WM_MBUTTONDOWN: case WM_MBUTTONUP:
		case WM_XBUTTONDOWN: case WM_XBUTTONUP:
		case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
		case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
			return true;
		default: return false;
		}
	}

	LRESULT CALLBACK GameInputMessageHook(int code, WPARAM remove, LPARAM parameter)
	{
		if (code >= 0 && remove == PM_REMOVE && parameter) {
			const MSG* message = reinterpret_cast<const MSG*>(parameter);
			if (message->hwnd == g_inputWindow.load(std::memory_order_relaxed) &&
				IsInputMessage(message->message)) {
				LARGE_INTEGER at{};
				if (QueryPerformanceCounter(&at)) {
					std::lock_guard<std::mutex> lock(g_timingLock);
					g_liveTiming.ObserveInput(static_cast<uint64_t>(at.QuadPart));
				}
			}
		}
		return CallNextHookEx(nullptr, code, remove, parameter);
	}

	void* OriginalSlot(IDXGISwapChain* chain, size_t index)
	{
		void** vtable = *reinterpret_cast<void***>(chain);
		std::lock_guard<std::mutex> lock(g_slotsLock);
		return g_slots.Find(&vtable[index]);
	}

	HRESULT CallPresent(IDXGISwapChain* chain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS*)
	{
		auto original = reinterpret_cast<PresentFn>(OriginalSlot(chain, 8));
		return original ? original(chain, sync, flags) : DXGI_ERROR_INVALID_CALL;
	}

	HRESULT CallPresent1(IDXGISwapChain* chain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
	{
		auto original = reinterpret_cast<Present1Fn>(OriginalSlot(chain, 22));
		return original ? original(static_cast<IDXGISwapChain1*>(chain), sync, flags, parameters) : DXGI_ERROR_INVALID_CALL;
	}

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
		LARGE_INTEGER appEntry{};
		QueryPerformanceCounter(&appEntry);
		HRESULT hr;
		bool ours = false;
		if (!g_inHook)
		{
			g_inHook = true;
			ours = FgPresent(sc, sync, flags, original, parameters, &hr);
			g_presents++;
			if (!ours) hr = original(sc, sync, flags, parameters);
			if (hr == S_OK && !(flags & DXGI_PRESENT_TEST)) {
				DXGI_SWAP_CHAIN_DESC desc{};
				if (SUCCEEDED(sc->GetDesc(&desc)) && IsGameFpsWindow(desc.OutputWindow)) {
					CountPresentFrames(1, 0);
					RecordGamePresentQpc(static_cast<uint64_t>(appEntry.QuadPart));
					StartGameInputHook(desc.OutputWindow);
				}
			}
			g_inHook = false;
			return hr;
		}
		return original(sc, sync, flags, parameters);
	}

	HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* sc, UINT sync, UINT flags)
	{
		return PresentWrapped(sc, sync, flags, &CallPresent, nullptr);
	}

	HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
	{
		return PresentWrapped(sc, sync, flags, &CallPresent1, p);
	}

	bool Patch(void** slot, void* hook)
	{
		std::lock_guard<std::mutex> lock(g_slotsLock);
		if (g_slots.Find(slot)) return true; // same vtable already hooked
		if (*slot == hook) { BridgeLog("Present hook: slot already points at our callback but no original is registered."); return false; }
		DWORD was = 0;
		if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &was)) { BridgeLog("Present hook: VirtualProtect failed (error %lu).", GetLastError()); return false; }
		g_slots.Remember(slot, *slot); // visible before the vtable pointer is replaced
		InterlockedExchangePointer(slot, hook);
		VirtualProtect(slot, sizeof(void*), was, &was);
		return true;
	}

	void HookChain(IDXGISwapChain* chain)
	{
		// The actual Unity chain and a temporary chain can have different vtables.
		auto vtable = *reinterpret_cast<void***>(chain);
		const bool present = Patch(&vtable[8], reinterpret_cast<void*>(&PresentHook));
		ComPtr<IDXGISwapChain1> chain1;
		bool present1 = false;
		if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1)))) {
			void** vtable1 = *reinterpret_cast<void***>(chain1.Get());
			Patch(&vtable1[8], reinterpret_cast<void*>(&PresentHook));
			present1 = Patch(&vtable1[22], reinterpret_cast<void*>(&Present1Hook));
		}
		if (present || present1) g_hooked = true;
		BridgeLog("Present hook: Present=%d Present1=%d.", present, present1);
	}

	bool UnityWindow(HWND window)
	{
		wchar_t name[64]{};
		return GetClassNameW(window, name, _countof(name)) && wcscmp(name, L"UnityWndClass") == 0;
	}

	void CaptureChain(HWND window, IUnknown* device, IDXGISwapChain* chain)
	{
		ComPtr<ID3D11Device> d11;
		// The AMD provider also creates a chain for this HWND, using a D3D12 queue.
		if (UnityWindow(window) && device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d11)))) {
			g_window = window;
			HookChain(chain);
		}
	}

	bool TryNative(IDXGIFactory2* factory, IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1& desc,
		const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGISwapChain1** chain)
	{
		if (!g_tryNative || !device || !chain || !UnityWindow(window)) return false;
		ComPtr<ID3D11Device> d11;
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(&d11)))) return false;
		const HRESULT hr = CreateNativeD3D11Proxy(factory, d11.Get(), window, desc, fullscreen, chain);
		if (FAILED(hr)) {
			BridgeLog("AMD native SwapChain unavailable (0x%08x, format %u, swap effect %u); keeping manual FG.",
				static_cast<unsigned>(hr), desc.Format, desc.SwapEffect);
			return false;
		}
		g_window = window;
		StartGameInputHook(window);
		return true;
	}

	HRESULT STDMETHODCALLTYPE CreateHook(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
		IDXGISwapChain** chain)
	{
		if (g_tryNative && desc && chain && UnityWindow(desc->OutputWindow)) {
			ComPtr<IDXGIFactory2> factory2;
			if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2)))) {
				DXGI_SWAP_CHAIN_DESC1 translated = {};
				translated.Width = desc->BufferDesc.Width;
				translated.Height = desc->BufferDesc.Height;
				translated.Format = desc->BufferDesc.Format;
				translated.SampleDesc = desc->SampleDesc;
				translated.BufferUsage = desc->BufferUsage;
				translated.BufferCount = desc->BufferCount;
				translated.SwapEffect = desc->SwapEffect;
				translated.Flags = desc->Flags;
				DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen = {desc->BufferDesc.RefreshRate,
					desc->BufferDesc.ScanlineOrdering, desc->BufferDesc.Scaling, desc->Windowed};
				IDXGISwapChain1* native = nullptr;
				if (TryNative(factory2.Get(), device, desc->OutputWindow, translated, &fullscreen, &native)) {
					*chain = native;
					return S_OK;
				}
			}
		}
		const HRESULT hr = g_create(factory, device, desc, chain);
		if (SUCCEEDED(hr) && desc && chain && *chain) CaptureChain(desc->OutputWindow, device, *chain);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE CreateForHwndHook(IDXGIFactory2* factory, IUnknown* device, HWND window,
		const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
		IDXGIOutput* output, IDXGISwapChain1** chain)
	{
		if (desc && !output && TryNative(factory, device, window, *desc, fullscreen, chain)) return S_OK;
		const HRESULT hr = g_createForHwnd(factory, device, window, desc, fullscreen, output, chain);
		if (SUCCEEDED(hr) && chain && *chain) CaptureChain(window, device, *chain);
		return hr;
	}

	void Hook(ID3D11Resource* any)
	{
		// A temporary window or DXGI chain can fail while Unity is starting up.
		// Keep probing after a cooldown until at least one vtable is hooked.
		if (g_hooked || !g_probeRetry.Allow(GetTickCount64())) return;
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

bool CaptureGameSwapChain(const GakuHookService& hooks, bool nativeSwapchain)
{
	ComPtr<IDXGIFactory2> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
	auto vtable = *reinterpret_cast<void***>(factory.Get());
	GakuHookRequest requests[]{
		{vtable[10], reinterpret_cast<void*>(&CreateHook), reinterpret_cast<void**>(&g_create)},
		{vtable[15], reinterpret_cast<void*>(&CreateForHwndHook), reinterpret_cast<void**>(&g_createForHwnd)}
	};
	g_capture = hooks.acquireBatch(hooks.context, _countof(requests), requests) == GAKU_OK;
	g_tryNative = g_capture && nativeSwapchain;
	return g_capture;
}

void InstallPresentHook(ID3D11Resource* anchor)
{
	// Also probe a temporary chain when factory hooks were installed after
	// Unity's swap chain already existed. Patch() deduplicates shared vtables.
	Hook(anchor);
}

bool IsPresentWindow(HWND window) {
	if (HWND known = g_window.load()) return known == window;
	// The factory hook can fail even when the fallback Present vtable hook works.
	// Never treat that as permission to interpolate an unrelated swap chain.
	if (!UnityWindow(window)) return false;
	g_window = window;
	return true;
}
bool PresentHooked() { return g_hooked; }
uint64_t PresentsSeen() { return g_presents.load(); }

bool IsGameFpsWindow(void* window) {
	if (const HWND known = g_window.load()) return known == window;
	return UnityWindow(static_cast<HWND>(window));
}

void CountPresentFrames(uint64_t real, uint64_t generated) {
	if(real) g_realFrames.fetch_add(real, std::memory_order_relaxed);
	if(generated) g_generatedFrames.fetch_add(generated, std::memory_order_relaxed);
}

PresentCounts ReadPresentCounts() {
	return {g_realFrames.load(std::memory_order_relaxed), g_generatedFrames.load(std::memory_order_relaxed)};
}

void RecordGamePresentQpc(uint64_t at) {
	LARGE_INTEGER frequency{};
	if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return;
	std::lock_guard<std::mutex> lock(g_timingLock);
	g_liveTiming.ObservePresent(at, static_cast<uint64_t>(frequency.QuadPart));
}

LiveTimingSummary ReadLiveTiming() {
	LARGE_INTEGER now{}, frequency{};
	if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return {};
	std::lock_guard<std::mutex> lock(g_timingLock);
	auto result = g_liveTiming.Snapshot(static_cast<uint64_t>(now.QuadPart),
		static_cast<uint64_t>(frequency.QuadPart));
	result.inputHookActive = g_inputWindow.load(std::memory_order_relaxed) != nullptr;
	return result;
}

void StartGameInputHook(void* window) {
	HWND hwnd = static_cast<HWND>(window);
	if (!hwnd || g_inputWindow.load(std::memory_order_relaxed) == hwnd) return;
	DWORD processId = 0;
	const DWORD threadId = GetWindowThreadProcessId(hwnd, &processId);
	if (!threadId || processId != GetCurrentProcessId()) return;
	std::lock_guard<std::mutex> lock(g_inputHookLock);
	if (g_inputWindow.load(std::memory_order_relaxed) == hwnd) return;
	const uint64_t now = GetTickCount64();
	if (now < g_nextInputHookAttempt) return;
	if (g_inputHook) {
		UnhookWindowsHookEx(g_inputHook);
		g_inputHook = nullptr;
		g_inputWindow = nullptr;
	}
	// Thread-local to this process's Unity window; never install a desktop-wide hook.
	g_inputHook = SetWindowsHookExW(WH_GETMESSAGE, &GameInputMessageHook, nullptr, threadId);
	if (!g_inputHook) {
		g_nextInputHookAttempt = now + 5000;
		BridgeLog("Input timing: Unity message hook unavailable (error %lu).", GetLastError());
		return;
	}
	g_inputWindow = hwnd;
	g_nextInputHookAttempt = 0;
}

void StopGameInputHook() {
	std::lock_guard<std::mutex> hookLock(g_inputHookLock);
	g_inputWindow = nullptr;
	if (g_inputHook) {
		UnhookWindowsHookEx(g_inputHook);
		g_inputHook = nullptr;
	}
	g_nextInputHookAttempt = 0;
	std::lock_guard<std::mutex> timingLock(g_timingLock);
	g_liveTiming.Reset();
}
