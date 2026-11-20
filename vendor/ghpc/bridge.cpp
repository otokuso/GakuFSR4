// GPL-3.0: the D3D11/D3D12 bridge is from marsattackk/GHPC-FSR4.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "../../src/FgDistortionMath.h"

#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "dx12/ffx_api_dx12.h"
#include "dx12/ffx_api_framegeneration_dx12.h"
#include "ffx_upscale.h"
#include "ffx_framegeneration.h"
#include "../../src/FsrProvider.h"
#define GAKU_FSR_BRIDGE_IMPLEMENTATION
#include "../../src/FsrBridge.h"
#undef GAKU_FSR_BRIDGE_IMPLEMENTATION
#include "../../src/PresentHook.h"
#include "../../src/FgErrorBurst.h"
#include "../../src/FgRetry.h"
#include "../../src/FgViewportMath.h"
#include "../../src/ExposurePolicy.h"
#include "../../src/FgCameraValidation.h"
#include "../../src/NativeSwapchain.h"
#include "../../src/PresentRate.h"
#include "../../src/LiveTiming.h"
#include "../../src/DafpDiagnostics.h"

using Microsoft::WRL::ComPtr;

static_assert(sizeof(void*) == 8, "64-bit only");

struct LayoutField { const char* name; uint32_t offset; };
#define LF(T, f) { #f, (uint32_t)offsetof(T, f) }
static const LayoutField kFrameFields[] = {
	LF(FrameData, color), LF(FrameData, depth), LF(FrameData, motion), LF(FrameData, output),
	LF(FrameData, renderW), LF(FrameData, renderH), LF(FrameData, displayW), LF(FrameData, displayH),
	LF(FrameData, jitterX), LF(FrameData, jitterY), LF(FrameData, mvScaleX), LF(FrameData, mvScaleY),
	LF(FrameData, frameTimeMs), LF(FrameData, cameraNear), LF(FrameData, cameraFar), LF(FrameData, fovV),
	LF(FrameData, sharpness), LF(FrameData, reset), LF(FrameData, createFlags), LF(FrameData, dispatchFlags),
	LF(FrameData, exposure), LF(FrameData, direct), LF(FrameData, maxRenderW), LF(FrameData, maxRenderH),
	LF(FrameData, hudless), LF(FrameData, fg), LF(FrameData, camPos), LF(FrameData, camUp), LF(FrameData, camRight), LF(FrameData, camFwd),
};
static const LayoutField kStatsFields[] = {
	LF(Stats, frames), LF(Stats, providerId), LF(Stats, lastResult), LF(Stats, failed), LF(Stats, provider),
	LF(Stats, adapter), LF(Stats, presented), LF(Stats, cpuMs), LF(Stats, gpuMs), LF(Stats, fsrMemory),
	LF(Stats, logDropped), LF(Stats, bridgeStarts), LF(Stats, contextStarts), LF(Stats, removedReason), LF(Stats, failures),
	LF(Stats, gpuPeriodMs), LF(Stats, gpuPeriodMin), LF(Stats, gpuPeriodMax), LF(Stats, gpuBusyMs), LF(Stats, gpuBusyMin),
	LF(Stats, gpuBusyMax), LF(Stats, gpuFrames), LF(Stats, gpuDisjoint), LF(Stats, gpuDropped), LF(Stats, markMs),
	LF(Stats, flushMs), LF(Stats, exposureWanted), LF(Stats, directFrames),
	LF(Stats, fgProvider), LF(Stats, fgProviderId), LF(Stats, fgPrepared), LF(Stats, fgGenerated), LF(Stats, fgReal), LF(Stats, fgSkipped),
	LF(Stats, fgMemory), LF(Stats, fgLastResult), LF(Stats, fgGpuMs), LF(Stats, fgPaceMs), LF(Stats, fgIntervalMs),
	LF(Stats, fgHookMs), LF(Stats, fgRefreshHz), LF(Stats, fgNotNeeded), LF(Stats, presentsHooked), LF(Stats, fgHudless),
};
#undef LF

namespace
{
	std::mutex g_logLock;
	std::deque<std::string> g_log;
	uint64_t g_logDropped = 0, g_logDroppedReported = 0;

	void Log(const char* fmt, ...)
	{
		char buf[1024];
		va_list a;
		va_start(a, fmt);
		vsnprintf(buf, sizeof buf, fmt, a);
		va_end(a);
		std::lock_guard<std::mutex> l(g_logLock);
		if (g_log.size() < 512) g_log.emplace_back(buf);
		else g_logDropped++;
	}

	std::mutex g_ffxMsgLock;
	std::map<std::string, uint64_t> g_ffxMsgCount;

	void OnFfxMessage(uint32_t type, const wchar_t* message)
	{
		char buf[1024]{};
		if (!message || !WideCharToMultiByte(CP_UTF8, 0, message, -1, buf, sizeof buf, nullptr, nullptr))
			snprintf(buf, sizeof buf, "(message unavailable)");
		const char* kind = type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning";
		uint64_t n;
		{
			std::lock_guard<std::mutex> l(g_ffxMsgLock);
			n = ++g_ffxMsgCount[std::string(kind) + buf];
		}
		if (n == 1) Log("FSR %s: %s", kind, buf);
		else
		{
			uint64_t p = 10;
			while (p < n) p *= 10;
			if (p == n) Log("FSR %s, %llu times so far: %s", kind, (unsigned long long)n, buf);
		}
	}

	std::mutex g_viewLock;
	uint64_t g_viewGen = 0, g_hudlessGen = 0;
	struct Retired { ComPtr<ID3D11Texture2D> t11; ComPtr<ID3D12Resource> t12; ComPtr<ID3D11ShaderResourceView> srv; uint64_t gen; uint64_t unusedSince; };
	std::vector<Retired> g_retired;
	uint64_t g_viewTicks = 0;

	struct Shared
	{
		ComPtr<ID3D11Texture2D> t11;
		ComPtr<ID3D12Resource> t12;
		ComPtr<ID3D11ShaderResourceView> srv;
		UINT w = 0, h = 0;
		DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
		void Release()
		{
			if (srv)
			{
				std::lock_guard<std::mutex> l(g_viewLock);
				g_retired.push_back({ t11, t12, srv, g_viewGen, 0 });
				g_viewGen++;
				srv.Reset();
			}
			t12.Reset(); t11.Reset(); w = h = 0; fmt = DXGI_FORMAT_UNKNOWN;
		}
	};

	using ImageRect = FgImageRect;

	const int Slots = 6;
	const DWORD FrameWaitMs = 2000, IdleWaitMs = 5000;

	struct State
	{
		HMODULE upscalerDll = nullptr, loaderDll = nullptr, fgDll = nullptr;
		ffxFunctions fx = {};
		bool loaded = false;
		std::atomic<bool> failed{ false };
		bool teardownDue = false;
		std::atomic<bool> fatal{ false };

		ID3D11Device* d11Base = nullptr;
		ComPtr<ID3D11Device5> d11;
		ComPtr<ID3D11DeviceContext4> ctx;
		ComPtr<ID3D12Device> d12;
		ComPtr<ID3D12CommandQueue> queue;
		ComPtr<ID3D12CommandAllocator> alloc[Slots];
		ComPtr<ID3D12GraphicsCommandList> list[Slots];
		uint64_t allocFence[Slots] = {};
		bool slotTimed[Slots] = {};
		ComPtr<ID3D11Fence> in11;
		ComPtr<ID3D12Fence> in12;
		ComPtr<ID3D12Fence> out12;
		ComPtr<ID3D11Fence> out11;
		uint64_t inValue = 0, outValue = 0;
		HANDLE event = nullptr;
		uint64_t submits = 0;
		ComPtr<ID3D12QueryHeap> timestamps;
		ComPtr<ID3D12Resource> readback;
		double tickMs = 0;

		Shared color, depth, motion, output;

		ffxContext fsr = nullptr;
		ComPtr<ID3D12Resource> expTex;
		ComPtr<ID3D12Resource> expUpload;
		ComPtr<ID3D12Resource> distortionTex, distortionUpload;
		float distortionK1 = 0, distortionK2 = 0, distortionCenterX = .5f, distortionCenterY = .5f;
		bool distortionAttempted = false, distortionDisabled = false;
		float expValue = 0;
		bool exposureRequired = false;
		uint64_t providerGen = 0;
		std::string wantProvider = "4."; // Auto-select latest FSR 4.x, never silently select FSR 3.
		uint64_t overrideId = 0;
		bool providersListed = false;
		uint32_t ctxDisplayW = 0, ctxDisplayH = 0, ctxRenderW = 0, ctxRenderH = 0, ctxFlags = 0;
		int errors = 0;
		Stats stats = {};

		ffxContext fg = nullptr;
		uint32_t fgDisplayW = 0, fgDisplayH = 0, fgRenderW = 0, fgRenderH = 0, fgFlags = 0;
		bool fgProvidersListed = false;
		bool fgUnavailable = false;
		FgRetry fgRetry;
		ImageRect fgContent{};
		bool fgViewportOverrideEnabled = false;
		ImageRect fgViewportOverride{};
		Shared present;
		uint64_t fgFrameId = 0;
		bool fgPending = false;
		bool fgHavePrevious = false;
		FgErrorBurst fgErrors;
		LONGLONG fgLastEntry = 0;
		double fgIntervalTicks = 0;
		bool fgOffLogged = true;
	};
	// The host never unloads plugins. COM teardown under the DLL loader lock can fail-fast in the driver.
	State& g = *new State;
	std::mutex g_renderLock;
	std::atomic<bool> g_nativeSwapchainActive{false};
	std::atomic<HWND> g_dafpWindow{nullptr};
	std::atomic<uint32_t> g_dafpSwapchainMajor{0};
	std::atomic<uint32_t> g_dafpSyncInterval{0};
	std::atomic<uint64_t> g_dafpLastPresentTick{0};
	std::atomic<uint64_t> g_dafpLastGeneratedTick{0};
	struct NativePresentState {
		IDXGISwapChain4* chain = nullptr;
		DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
		ffxContext swapchainContext = nullptr;
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		uint64_t fence = 0;
		uint64_t timingFence = 0;
		bool timingRequested = false;
		bool timingRecorded = false;
		bool timingValid = false;
		bool timingDisabled = false;
		std::atomic<uint64_t> generated{0};
		std::atomic<ffxReturnCode_t> error{FFX_API_RETURN_OK};
		bool enabled = false;
	};
	NativePresentState& nativePresent = *new NativePresentState;
	std::atomic<const char*> g_fgSkipReason{nullptr};

	void FgFailure(int stage, const char* name, ffxReturnCode_t code, uint64_t frameId)
	{
		g_fgSkipReason = "FG provider rejected the current frame";
		if (g.fgErrors.Record(stage, (uint32_t)code))
			Log("FG %s failed: %u (frame %llu).", name, (unsigned)code, (unsigned long long)frameId);
	}

	void BackoffFgFailure()
	{
		if (!g.fgRetry.Failed(GetTickCount64(), false)) {
			g_fgSkipReason = "FG retries exhausted until scene/backbuffer resize or device change";
			Log("FG provider failures exhausted retry budget; continuing FSR upscaling without FG.");
		}
	}

	void FgRecovered()
	{
		g.fgErrors.Reset();
	}

	std::mutex g_statsLock;
	Stats g_published = {};

	void Publish()
	{
		std::lock_guard<std::mutex> l(g_statsLock);
		g.stats.presentsHooked = PresentsSeen();
		g_published = g.stats;
	}

	void Average(float& avg, double sample)
	{
		avg = avg == 0.0f ? (float)sample : avg + ((float)sample - avg) / 32.0f;
	}

	DXGI_FORMAT Typed(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
		case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
		case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
		case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
		default: return f;
		}
	}

	bool Typeless(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
			return true;
		default: return false;
		}
	}

	bool made = false;
	bool deviceLost = false;

	void CheckRemoved()
	{
		HRESULT r12 = g.d12 ? g.d12->GetDeviceRemovedReason() : S_OK;
		HRESULT r11 = g.d11 ? g.d11->GetDeviceRemovedReason() : S_OK;
		HRESULT r = FAILED(r12) ? r12 : r11;
		if (SUCCEEDED(r)) return;
		deviceLost = true;
		if (g.stats.removedReason == (int32_t)r) return;
		g.stats.removedReason = (int32_t)r;
		Log("Device removed (%s): 0x%08X.", FAILED(r12) ? "our D3D12 device" : "the game's D3D11 device", (unsigned)r);
	}

	bool Check(HRESULT hr, const char* what)
	{
		if (SUCCEEDED(hr)) return true;
		Log("%s failed: 0x%08X", what, (unsigned)hr);
		CheckRemoved();
		return false;
	}

	bool WaitOut(uint64_t value, DWORD ms, const char* what)
	{
		if (!value || g.out12->GetCompletedValue() >= value) return true;
		if (!Check(g.out12->SetEventOnCompletion(value, g.event), "SetEventOnCompletion")) return false;
		DWORD w = WaitForSingleObject(g.event, ms);
		if (w == WAIT_OBJECT_0) return true;
		Log("%s: the GPU did not finish within %lu ms (wait result %lu, fence at %llu of %llu).", what, ms, w,
			(unsigned long long)g.out12->GetCompletedValue(), (unsigned long long)value);
		CheckRemoved();
		deviceLost = true;
		return false;
	}

	bool WaitIdle(const char* what, bool evenIfLost = false)
	{
		if (!g.queue || !g.out12) return true;
		if (deviceLost && !evenIfLost) return false;
		uint64_t v = ++g.outValue;
		if (!Check(g.queue->Signal(g.out12.Get(), v), "queue Signal")) return false;
		return WaitOut(v, IdleWaitMs, what);
	}

	void ReleaseShared()
	{
		g.color.Release();
		g.depth.Release();
		g.motion.Release();
		g.output.Release();
		g.present.Release();
	}

	void DestroyFg()
	{
		FgRecovered();
		if (g.fg && g.fx.DestroyContext)
		{
			ffxConfigureDescFrameGeneration c = {};
			c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
			c.frameGenerationEnabled = false;
			c.swapChain = nativePresent.chain;
			if (nativePresent.chain) {
				ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait = {};
				wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
				g.fx.Dispatch(&nativePresent.swapchainContext, &wait.header);
				nativePresent.enabled = false;
			}
			c.frameID = g.fgFrameId;
			g.fx.Configure(&g.fg, &c.header);
			g.fx.DestroyContext(&g.fg, nullptr);
			Log("FG context destroyed (%llu frames prepared, %llu generated frames presented).",
				(unsigned long long)g.stats.fgPrepared, (unsigned long long)g.stats.fgGenerated);
		}
		g.fg = nullptr;
		g.fgDisplayW = g.fgDisplayH = g.fgRenderW = g.fgRenderH = g.fgFlags = 0;
		g.fgPending = g.fgHavePrevious = false;
		g.distortionTex.Reset();
		g.distortionUpload.Reset();
		g.distortionAttempted = g.distortionDisabled = false;
		g.fgLastEntry = 0;
		g.fgIntervalTicks = 0;
		g.stats.fgIntervalMs = g.stats.fgGpuMs = 0;
		nativePresent.timingValid = false;
		g.stats.fgProviderId = 0;
		g.stats.fgProvider[0] = 0;
		g.stats.fgMemory = 0;
	}

	void DestroyContext()
	{
		DestroyFg();
		if (g.fsr && g.fx.DestroyContext) g.fx.DestroyContext(&g.fsr, nullptr);
		g.fsr = nullptr;
		g.ctxDisplayW = g.ctxDisplayH = g.ctxRenderW = g.ctxRenderH = g.ctxFlags = 0;
		g.stats.providerId = 0;
		g.stats.provider[0] = 0;
		g.stats.fsrMemory = 0;
	}

	void Teardown(const char* why)
	{
		bool idle = (g.d12 && FAILED(g.d12->GetDeviceRemovedReason())) || WaitIdle("teardown", true);
		if (idle)
		{
			DestroyContext();
			ReleaseShared();
			for (int i = 0; i < Slots; i++) { g.list[i].Reset(); g.alloc[i].Reset(); }
			g.timestamps.Reset();
			g.readback.Reset();
			g.expTex.Reset();
			g.expUpload.Reset();
			g.in12.Reset();
			g.out12.Reset();
			g.queue.Reset();
			g.d12.Reset();
		}
		else
		{
			g.fsr = nullptr;
			g.fg = nullptr;
			g.fgDisplayW = g.fgDisplayH = g.fgRenderW = g.fgRenderH = g.fgFlags = 0;
			g.fgPending = g.fgHavePrevious = false;
			g.ctxDisplayW = g.ctxDisplayH = g.ctxRenderW = g.ctxRenderH = g.ctxFlags = 0;
			g.stats.providerId = 0;
			g.stats.provider[0] = 0;
			g.stats.fsrMemory = 0;
			g.stats.fgProviderId = 0;
			g.stats.fgProvider[0] = 0;
			for (Shared* s : { &g.color, &g.depth, &g.motion, &g.output, &g.present }) { s->t12.Detach(); s->Release(); }
			for (int i = 0; i < Slots; i++) { g.list[i].Detach(); g.alloc[i].Detach(); }
			g.timestamps.Detach();
			g.readback.Detach();
			g.expTex.Detach();
			g.expUpload.Detach();
			g.distortionTex.Detach(); g.distortionUpload.Detach();
			g.in12.Detach();
			g.out12.Detach();
			g.queue.Detach();
			g.d12.Detach();
		}
		for (int i = 0; i < Slots; i++) { g.allocFence[i] = 0; g.slotTimed[i] = false; }
		g.in11.Reset();
		g.out11.Reset();
		g.ctx.Reset();
		g.d11.Reset();
		g.d11Base = nullptr;
		g.fgContent = {};
		g.inValue = g.outValue = 0;
		g.expValue = 0;
		g.providersListed = false;
		g.overrideId = 0;
		g.exposureRequired = false;
		g.fgUnavailable = false;
		g.fgRetry.NewDevice();
		deviceLost = false;
		Log("Bridge shut down (%s)%s.", why, idle ? "" : "; its GPU objects abandoned, the queue was not idle");
	}

	bool InitDevices(ID3D11Device* dev)
	{
		g.d11Base = dev;
		if (!Check(dev->QueryInterface(IID_PPV_ARGS(&g.d11)), "ID3D11Device5 (Windows 10 1703 or later)")) return false;
		ComPtr<ID3D11DeviceContext> ic;
		dev->GetImmediateContext(&ic);
		if (!Check(ic.As(&g.ctx), "ID3D11DeviceContext4")) return false;

		ComPtr<IDXGIDevice> xd;
		ComPtr<IDXGIAdapter> ad;
		DXGI_ADAPTER_DESC ds = {};
		if (!Check(dev->QueryInterface(IID_PPV_ARGS(&xd)), "IDXGIDevice") || !Check(xd->GetAdapter(&ad), "GetAdapter") || !Check(ad->GetDesc(&ds), "GetDesc")) return false;
		WideCharToMultiByte(CP_UTF8, 0, ds.Description, -1, g.stats.adapter, sizeof g.stats.adapter, nullptr, nullptr);

		ComPtr<IDXGIFactory4> factory;
		ComPtr<IDXGIAdapter1> ad12;
		if (!Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2")) return false;
		if (!Check(UnwrapReShade(factory.Get())->EnumAdapterByLuid(ds.AdapterLuid, IID_PPV_ARGS(&ad12)), "EnumAdapterByLuid")) return false;
		// Creating the private device through ReShade resets AutoHDR's shared device state.
		const auto getInterface = reinterpret_cast<decltype(&D3D12GetInterface)>(
			GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12GetInterface"));
		constexpr GUID deviceFactoryClass = {0x114863bf, 0xc386, 0x4aee, {0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55}};
		ComPtr<ID3D12DeviceFactory> deviceFactory;
		if (getInterface && SUCCEEDED(getInterface(deviceFactoryClass, IID_PPV_ARGS(&deviceFactory))))
			Check(deviceFactory->CreateDevice(ad12.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.d12)),
				"ID3D12DeviceFactory::CreateDevice");
		if (!g.d12) {
			if (!Check(D3D12CreateDevice(ad12.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.d12)), "D3D12CreateDevice")) return false;
			g.d12 = UnwrapReShade(g.d12.Get());
		}

		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (!Check(g.d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)), "CreateCommandQueue")) return false;
		for (int i = 0; i < Slots; i++)
		{
			if (!Check(g.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc[i])), "CreateCommandAllocator")) return false;
			if (!Check(g.d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[i].Get(), nullptr, IID_PPV_ARGS(&g.list[i])), "CreateCommandList")) return false;
			if (!Check(g.list[i]->Close(), "command list Close")) return false;
		}

		D3D12_QUERY_HEAP_DESC qh = {};
		qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		qh.Count = 2 * (Slots + 1);
		if (!Check(g.d12->CreateQueryHeap(&qh, IID_PPV_ARGS(&g.timestamps)), "CreateQueryHeap")) return false;
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = 2 * (Slots + 1) * sizeof(uint64_t);
		bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (!Check(g.d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.readback)), "timestamp readback buffer")) return false;
		UINT64 freq = 0;
		if (!Check(g.queue->GetTimestampFrequency(&freq), "GetTimestampFrequency") || !freq) return false;
		g.tickMs = 1000.0 / (double)freq;

		HANDLE h = nullptr;
		if (!Check(g.d11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.in11)), "ID3D11Device5::CreateFence")) return false;
		if (!Check(g.in11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h), "in fence CreateSharedHandle")) return false;
		HRESULT hr = g.d12->OpenSharedHandle(h, IID_PPV_ARGS(&g.in12));
		CloseHandle(h);
		if (!Check(hr, "in fence OpenSharedHandle")) return false;
		if (!Check(g.d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.out12)), "ID3D12Device::CreateFence")) return false;
		h = nullptr;
		if (!Check(g.d12->CreateSharedHandle(g.out12.Get(), nullptr, GENERIC_ALL, nullptr, &h), "out fence CreateSharedHandle")) return false;
		hr = g.d11->OpenSharedFence(h, IID_PPV_ARGS(&g.out11));
		CloseHandle(h);
		if (!Check(hr, "out fence OpenSharedFence")) return false;
		if (!g.event && !(g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr))) { Log("CreateEventW failed: error %lu.", GetLastError()); return false; }

		g.stats.bridgeStarts++;
		Log("Bridge ready on %s: D3D12 device and queue, shared fences (start %u).", g.stats.adapter, g.stats.bridgeStarts);
		return true;
	}

	bool MakeShared(Shared& s, UINT w, UINT hgt, DXGI_FORMAT fmt, bool uav, const char* name, bool view, int srcFormat);

	bool EnsureShared(Shared& s, ID3D11Texture2D* src, bool uav, const char* name, bool view)
	{
		D3D11_TEXTURE2D_DESC sd;
		src->GetDesc(&sd);
		DXGI_FORMAT fmt = Typed(sd.Format);
		// A cache hit on dimensions/format does not make CopyResource legal if
		// the application's texture changed its sample or subresource layout.
		if (sd.SampleDesc.Count != 1) { Log("%s: multisampled textures are not supported.", name); return false; }
		if (sd.MipLevels != 1 || sd.ArraySize != 1) { Log("%s: %u mips, %u slices: CopyResource needs exactly one of each.", name, sd.MipLevels, sd.ArraySize); return false; }
		if (Typeless(fmt)) { Log("%s: no typed format for game texture format %d.", name, (int)sd.Format); return false; }
		if (s.t11 && s.w == sd.Width && s.h == sd.Height && s.fmt == fmt) return true;
		return MakeShared(s, sd.Width, sd.Height, fmt, uav, name, view, (int)sd.Format);
	}

	bool MakeShared(Shared& s, UINT w, UINT hgt, DXGI_FORMAT fmt, bool uav, const char* name, bool view, int srcFormat)
	{
		if (s.t11 && s.w == w && s.h == hgt && s.fmt == fmt) return true;
		if (!WaitIdle("new shared texture")) return false;
		s.Release();
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = w;
		d.Height = hgt;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = fmt;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
		d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		if (!Check(g.d11->CreateTexture2D(&d, nullptr, &s.t11), name)) return false;

		ComPtr<IDXGIResource1> r;
		HANDLE h = nullptr;
		if (!Check(s.t11.As(&r), "IDXGIResource1") || !Check(r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h), "texture CreateSharedHandle"))
			{ s.Release(); return false; }
		HRESULT hr = g.d12->OpenSharedHandle(h, IID_PPV_ARGS(&s.t12));
		CloseHandle(h);
		if (!Check(hr, "texture OpenSharedHandle")) { s.Release(); return false; }
		s.w = w;
		s.h = hgt;
		s.fmt = fmt;
		made = true;
		if (view)
		{
			ComPtr<ID3D11ShaderResourceView> srv;
			if (!Check(g.d11->CreateShaderResourceView(s.t11.Get(), nullptr, &srv), "shared texture view")) { s.Release(); return false; }
			std::lock_guard<std::mutex> l(g_viewLock);
			s.srv = srv;
			g_viewGen++;
		}
		Log("Shared %s %ux%u, format %d (game texture format %d)%s.", name, s.w, s.h, (int)fmt, srcFormat, view ? ", viewed by Unity" : "");
		return true;
	}

	bool PickProvider()
	{
		if (g.providersListed) return g.wantProvider.empty() || g.overrideId != 0;
		g.providersListed = true;
		uint64_t count = 0;
		ffxQueryDescGetVersions vq = {};
		vq.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
		vq.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		vq.device = g.d12.Get();
		vq.outputCount = &count;
		ffxReturnCode_t rc = g.fx.Query(nullptr, &vq.header);
		uint64_t ids[16] = {};
		const char* names[16] = {};
		if (rc == FFX_API_RETURN_OK && count)
		{
			if (count > 16) count = 16;
			vq.versionIds = ids;
			vq.versionNames = names;
			rc = g.fx.Query(nullptr, &vq.header);
		}
		if (rc != FFX_API_RETURN_OK) { Log("FSR providers: the version query failed (%u).", rc); return g.wantProvider.empty(); }
		std::string list;
		FsrVersion newest{};
		const FsrVersion pinned = g.wantProvider == "4." ? FsrVersion{} : ParseFsrProviderVersion(g.wantProvider.c_str());
		std::string selected;
		for (uint64_t i = 0; i < count; i++)
		{
			char item[96];
			snprintf(item, sizeof item, "%s'%s' (id 0x%llX)", i ? ", " : "", names[i] ? names[i] : "?", (unsigned long long)ids[i]);
			list += item;
			if (!names[i]) continue;
			FsrVersion candidate = ParseFsrProviderVersion(names[i]);
			if (g.wantProvider == "4.") {
				if (NewerFsr4Provider(candidate, newest)) { newest = candidate; selected = names[i]; g.overrideId = ids[i]; }
			} else if (candidate.valid && candidate.major == pinned.major &&
				candidate.minor == pinned.minor && candidate.patch == pinned.patch) {
				selected = names[i]; g.overrideId = ids[i];
			}
		}
		Log("FSR providers on this GPU (%llu): %s.", (unsigned long long)count, list.c_str());
		if (g.wantProvider.empty()) return true;
		if (!g.overrideId) { Log("FSR provider '%s' asked for: not offered here.", g.wantProvider.c_str()); return false; }
		Log("FSR provider %s: '%s' (id 0x%llX).", g.wantProvider == "4." ? "latest 4.x" : "pinned", selected.c_str(), (unsigned long long)g.overrideId);
		return true;
	}

	bool EnsureContext(const FrameData& fd)
	{
		bool dynamic = (fd.createFlags & FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION) != 0;
		uint32_t keyW = dynamic ? (fd.maxRenderW > fd.renderW ? fd.maxRenderW : fd.renderW) : fd.renderW;
		uint32_t keyH = dynamic ? (fd.maxRenderH > fd.renderH ? fd.maxRenderH : fd.renderH) : fd.renderH;
		if (g.fsr && g.ctxDisplayW == fd.displayW && g.ctxDisplayH == fd.displayH && g.ctxRenderW == keyW && g.ctxRenderH == keyH
			&& g.ctxFlags == fd.createFlags) return true;
		LARGE_INTEGER t0, t1, qf;
		QueryPerformanceCounter(&t0);
		if (!WaitIdle("new FSR context")) return false;
		DestroyContext();
		if (!PickProvider()) return false;

		ffxCreateBackendDX12Desc backend = {};
		backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backend.device = g.d12.Get();
		ffxCreateContextDescUpscaleVersion version = {};
		version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
		version.version = FFX_UPSCALER_VERSION;
		ffxCreateContextDescUpscale desc = {};
		desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		desc.flags = fd.createFlags;
		desc.maxRenderSize = { keyW, keyH };
		desc.maxUpscaleSize = { fd.displayW, fd.displayH };
		desc.fpMessage = OnFfxMessage;
		desc.header.pNext = &backend.header;
		backend.header.pNext = &version.header;
		ffxOverrideVersion force = {};
		force.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
		force.versionId = g.overrideId;
		if (g.overrideId) version.header.pNext = &force.header;

		ffxReturnCode_t rc = g.fx.CreateContext(&g.fsr, &desc.header, nullptr);
		if (rc != FFX_API_RETURN_OK) { Log("ffxCreateContext failed: %u.", rc); g.fsr = nullptr; CheckRemoved(); return false; }
		QueryPerformanceCounter(&t1);
		QueryPerformanceFrequency(&qf);
		double madeMs = (t1.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart;
		g.ctxDisplayW = fd.displayW;
		g.ctxDisplayH = fd.displayH;
		g.ctxRenderW = keyW;
		g.ctxRenderH = keyH;
		g.ctxFlags = fd.createFlags;
		g.stats.contextStarts++;
		made = true;

		ffxQueryGetProviderVersion pv = {};
		pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		if (g.fx.Query(&g.fsr, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
		{
			g.stats.providerId = pv.versionId;
			snprintf(g.stats.provider, sizeof g.stats.provider, "%s", pv.versionName);
		}
		FfxApiEffectMemoryUsage mem = {};
		ffxQueryDescUpscaleGetGPUMemoryUsage mq = {};
		mq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
		mq.gpuMemoryUsageUpscaler = &mem;
		ffxReturnCode_t mrc = g.fx.Query(&g.fsr, &mq.header);
		g.stats.fsrMemory = mrc == FFX_API_RETURN_OK ? mem.totalUsageInBytes : 0;
		Log("FSR context %ux%u -> %ux%u, flags 0x%X: provider '%s' (id 0x%llX); GPU memory %.1f MB (%.1f MB aliasable)%s; context %u, made in %.1f ms (queue idle, old one destroyed, new one created).",
			fd.renderW, fd.renderH, fd.displayW, fd.displayH, fd.createFlags, g.stats.provider, (unsigned long long)g.stats.providerId,
			mem.totalUsageInBytes / 1048576.0, mem.aliasableUsageInBytes / 1048576.0, mrc == FFX_API_RETURN_OK ? "" : " (query failed)",
			g.stats.contextStarts, madeMs);
		ffxQueryDescUpscaleGetResourceRequirements rq = {};
		rq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS;
		ffxReturnCode_t rrc = g.fx.Query(&g.fsr, &rq.header);
		if (rrc == FFX_API_RETURN_OK)
			Log("FSR inputs: required 0x%llX, optional 0x%llX (reactive mask %s, transparency-and-composition mask %s).",
				(unsigned long long)rq.required_resources, (unsigned long long)rq.optional_resources,
				(rq.required_resources & FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK) ? "required" : (rq.optional_resources & FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK) ? "optional" : "not used",
				(rq.required_resources & FFX_API_QUERY_RESOURCE_INPUT_TRANSPARENCYCOMPOSITION) ? "required" : (rq.optional_resources & FFX_API_QUERY_RESOURCE_INPUT_TRANSPARENCYCOMPOSITION) ? "optional" : "not used");
		else Log("FSR inputs: the resource-requirements query failed (%u).", rrc);
		if (rrc == FFX_API_RETURN_OK)
		{
			bool req = (rq.required_resources & FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE) != 0;
			if (req != g.exposureRequired) Log("FSR external exposure input %s by this provider; %s.", req ? "required" : "not required",
				req && !HasExternalExposure(req, fd.exposure) ? "no game exposure is captured, attempting SDK auto exposure" : "using the available exposure mode");
			g.exposureRequired = req;
			g.stats.exposureWanted = req ? 1 : 0;
		}
		return true;
	}

	void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to, D3D12_RESOURCE_BARRIER& b)
	{
		b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = r;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	}

	void Fail(const char* why)
	{
		Log("FSR stopped: %s", why);
		CheckRemoved();
		if (deviceLost || !WaitIdle("stopping after an error")) Teardown("after an error");
		else { DestroyContext(); ReleaseShared(); }
		g.errors = 0;
		g.stats.failures++;
		g.failed = true;
	}

	double ReadGpuTime(int slot)
	{
		D3D12_RANGE r = { slot * 2 * sizeof(uint64_t), (slot * 2 + 2) * sizeof(uint64_t) };
		void* p = nullptr;
		if (FAILED(g.readback->Map(0, &r, &p))) return 0;
		const uint64_t* t = (const uint64_t*)((const char*)p + r.Begin);
		const double elapsed = t[1] > t[0] ? (t[1] - t[0]) * g.tickMs : 0;
		D3D12_RANGE none = { 0, 0 };
		g.readback->Unmap(0, &none);
		return elapsed;
	}

	void ReadTimestamps(int slot)
	{
		if (!g.slotTimed[slot]) return;
		g.slotTimed[slot] = false;
		const auto elapsed = ReadGpuTime(slot);
		if (elapsed > 0) Average(g.stats.gpuMs, elapsed);
	}

	void ObserveFgInterval(LONGLONG entry, LONGLONG frequency)
	{
		if (g.fgLastEntry) {
			const auto ticks = entry - g.fgLastEntry;
			if (ticks > 0 && ticks < frequency) {
				const double delta = static_cast<double>(ticks);
				g.fgIntervalTicks = g.fgIntervalTicks == 0 ? delta : g.fgIntervalTicks + (delta - g.fgIntervalTicks) / 8.0;
			} else g.fgIntervalTicks = 0;
		}
		g.fgLastEntry = entry;
		g.stats.fgIntervalMs = static_cast<float>(g.fgIntervalTicks * 1000.0 / frequency);
	}

	bool EnsureExposure(ID3D12GraphicsCommandList* l, int slot, float value)
	{
		if (!g.expTex)
		{
			D3D12_HEAP_PROPERTIES hp = {};
			hp.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC td = {};
			td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			td.Width = td.Height = 1;
			td.DepthOrArraySize = td.MipLevels = 1;
			td.Format = DXGI_FORMAT_R32_FLOAT;
			td.SampleDesc.Count = 1;
			if (!Check(g.d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.expTex)), "exposure texture")) return false;
			hp.Type = D3D12_HEAP_TYPE_UPLOAD;
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = Slots * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
			bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (!Check(g.d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g.expUpload)), "exposure upload buffer")) return false;
			g.expValue = 0;
		}
		if (g.expValue == value) return true;
		bool first = g.expValue == 0;
		const UINT64 at = (UINT64)slot * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
		void* p = nullptr;
		D3D12_RANGE none = { 0, 0 };
		if (!Check(g.expUpload->Map(0, &none, &p), "exposure upload Map")) return false;
		*(float*)((char*)p + at) = value;
		D3D12_RANGE written = { (SIZE_T)at, (SIZE_T)at + sizeof(float) };
		g.expUpload->Unmap(0, &written);
		D3D12_RESOURCE_BARRIER b;
		if (!first) { Barrier(g.expTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST, b); l->ResourceBarrier(1, &b); }
		D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
		dst.pResource = g.expTex.Get();
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.pResource = g.expUpload.Get();
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint.Offset = at;
		src.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
		l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		Barrier(g.expTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, b);
		l->ResourceBarrier(1, &b);
		g.expValue = value;
		return true;
	}

	struct Handed { float nearM, farM, sharpness, preExposure, exposure; uint32_t createFlags, dispatchFlags; };
	// Leftover, nothing reads g_handed in this build. Does no harm.
	Handed g_handed = { -1 };

	void TimingMark(int code);
	const uint32_t DirectColor = 1, DirectOutput = 2, DirectMotion = 4;

	void ListFgProviders()
	{
		if (g.fgProvidersListed) return;
		g.fgProvidersListed = true;
		uint64_t count = 0;
		ffxQueryDescGetVersions vq = {};
		vq.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
		vq.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
		vq.device = g.d12.Get();
		vq.outputCount = &count;
		ffxReturnCode_t rc = g.fx.Query(nullptr, &vq.header);
		uint64_t ids[16] = {};
		const char* names[16] = {};
		if (rc == FFX_API_RETURN_OK && count)
		{
			if (count > 16) count = 16;
			vq.versionIds = ids;
			vq.versionNames = names;
			rc = g.fx.Query(nullptr, &vq.header);
		}
		if (rc != FFX_API_RETURN_OK) { Log("FG providers: the version query failed (%u).", rc); return; }
		std::string list;
		for (uint64_t i = 0; i < count; i++)
		{
			char item[96];
			snprintf(item, sizeof item, "%s'%s' (id 0x%llX)", i ? ", " : "", names[i] ? names[i] : "?", (unsigned long long)ids[i]);
			list += item;
		}
		Log("FG providers on this GPU (%llu): %s.", (unsigned long long)count, list.c_str());
	}

	// Optional, user-calibrated radial UV offset field for the FG optical-flow
	// input. Its texture is created only if coefficients are nonzero; the game
	// must still supply the *actual* post-effect transform for full accuracy.
	bool EnsureFgDistortion(ID3D12GraphicsCommandList* list)
	{
		if ((!g.distortionK1 && !g.distortionK2) || g.distortionDisabled) return false;
		if (g.distortionTex) return true;
		if (g.distortionAttempted) return false;
		g.distortionAttempted = true;
		const UINT width=g.fgDisplayW, height=g.fgDisplayH;
		if (!width || !height || uint64_t(width)*height > 3840ull*2160ull) {
			Log("FG distortion field: size %ux%u exceeds the supported 4K limit.", width, height);
			return false;
		}
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC tex = {};
		tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		tex.Width = width; tex.Height = height;
		tex.DepthOrArraySize = tex.MipLevels = 1;
		tex.Format = DXGI_FORMAT_R32G32_FLOAT;
		tex.SampleDesc.Count = 1;
		ComPtr<ID3D12Resource> field, upload;
		if (!Check(g.d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&field)), "FG distortion texture")) return false;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
		UINT rows = 0;
		UINT64 bytes = 0;
		g.d12->GetCopyableFootprints(&tex, 0, 1, 0, &layout, &rows, nullptr, &bytes);
		if (!bytes || rows != height) return false;
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC buffer = {};
		buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buffer.Width = bytes;
		buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
		buffer.SampleDesc.Count = 1;
		buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (!Check(g.d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "FG distortion upload")) return false;
		void* mapped = nullptr;
		const D3D12_RANGE none = {0,0};
		if (!Check(upload->Map(0, &none, &mapped), "FG distortion upload Map")) return false;
		for (UINT y=0;y<height;++y) {
			float* row=reinterpret_cast<float*>(static_cast<char*>(mapped) + layout.Offset + UINT64(y)*layout.Footprint.RowPitch);
			for (UINT x=0;x<width;++x) {
				const float u=(float(x)+.5f)/width, v=(float(y)+.5f)/height;
				const auto offset=fg_distortion::RadialOffset(u,v,g.distortionCenterX,g.distortionCenterY,g.distortionK1,g.distortionK2);
				row[x*2]=offset.x; row[x*2+1]=offset.y;
			}
		}
		const D3D12_RANGE written = {0,static_cast<SIZE_T>(bytes)};
		upload->Unmap(0, &written);
		D3D12_TEXTURE_COPY_LOCATION dest={}, source={};
		dest.pResource=field.Get(); dest.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		source.pResource=upload.Get(); source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		source.PlacedFootprint=layout;
		list->CopyTextureRegion(&dest,0,0,0,&source,nullptr);
		D3D12_RESOURCE_BARRIER barrier;
		Barrier(field.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,barrier);
		list->ResourceBarrier(1,&barrier);
		g.distortionTex=field; g.distortionUpload=upload;
		Log("FG distortion: uploaded radial UV offsets at %ux%u (k1=%.3f, k2=%.3f).",width,height,g.distortionK1,g.distortionK2);
		return true;
	}

	bool EnsureFg(const FrameData& fd, uint32_t keyW, uint32_t keyH)
	{
		DXGI_SWAP_CHAIN_DESC1 chain = {};
		if (FAILED(nativePresent.chain->GetDesc1(&chain))) return false;
		const UINT displayW = chain.Width, displayH = chain.Height;
		g.fgContent = g.fgViewportOverrideEnabled ? g.fgViewportOverride : FgAspectFit(displayW, displayH, fd.displayW, fd.displayH);
		if (!FgViewportWithinBackbuffer(g.fgContent, displayW, displayH, fd.displayW, fd.displayH)) {
			g_fgSkipReason = "FG viewport is incompatible with the native SwapChain";
			return false;
		}
		uint32_t flags = (fd.createFlags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0;
		if (chain.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || nativePresent.colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
			flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
		if (g.fg && g.fgDisplayW == displayW && g.fgDisplayH == displayH && g.fgRenderW == keyW && g.fgRenderH == keyH && g.fgFlags == flags) return true;
		if (!WaitIdle("new FG context")) return false;
		DestroyFg();
		ListFgProviders();
		ffxCreateBackendDX12Desc backend = {};
		backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backend.device = g.d12.Get();
		ffxCreateContextDescFrameGenerationVersion version = {};
		version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
		version.version = FFX_FRAMEGENERATION_VERSION;
		ffxCreateContextDescFrameGeneration desc = {};
		desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
		desc.flags = flags;
		desc.displaySize = { displayW, displayH };
		desc.maxRenderSize = { keyW, keyH };
		desc.backBufferFormat = ffxApiGetSurfaceFormatDX12(chain.Format);
		desc.header.pNext = &backend.header;
		backend.header.pNext = &version.header;
		ffxReturnCode_t rc = g.fx.CreateContext(&g.fg, &desc.header, nullptr);
		if (rc != FFX_API_RETURN_OK) {
			const bool unsupported = rc == FFX_API_RETURN_NO_PROVIDER ||
				rc == FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE ||
				rc == FFX_API_RETURN_ERROR_PARAMETER ||
				rc == FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE;
			const uint32_t delay = g.fgRetry.Failed(GetTickCount64(), unsupported);
			Log("FG ffxCreateContext failed: %u. %s Continuing with upscaling.", rc,
				unsupported ? "Provider/configuration unsupported; off until the device changes." :
				delay ? "Will retry after a cooldown." : "Retries exhausted until a resize/device change.");
			g.fg = nullptr;
			g.fgUnavailable = unsupported;
			g_fgSkipReason = unsupported ? "FG provider or configuration unsupported" : "FG context creation failed";
			CheckRemoved();
			return false;
		}
		g.fgRetry.Recovered();
		g.fgDisplayW = displayW; g.fgDisplayH = displayH; g.fgRenderW = keyW; g.fgRenderH = keyH; g.fgFlags = flags;
		g.fgHavePrevious = false;
		made = true;
		ffxConfigureDescGlobalDebug dbg = {};
		dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
		dbg.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION;
		dbg.fpMessage = OnFfxMessage;
		g.fx.Configure(&g.fg, &dbg.header);
		ffxQueryGetProviderVersion pv = {};
		pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		if (g.fx.Query(&g.fg, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
		{
			g.stats.fgProviderId = pv.versionId;
			snprintf(g.stats.fgProvider, sizeof g.stats.fgProvider, "%s", pv.versionName);
		}
		FfxApiEffectMemoryUsage mem = {};
		ffxQueryDescFrameGenerationGetGPUMemoryUsage mq = {};
		mq.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
		mq.gpuMemoryUsageFrameGeneration = &mem;
		ffxReturnCode_t mrc = g.fx.Query(&g.fg, &mq.header);
		g.stats.fgMemory = mrc == FFX_API_RETURN_OK ? mem.totalUsageInBytes : 0;
		Log("FG context: inputs up to %ux%u, display %ux%u, flags 0x%X: provider '%s' (id 0x%llX); GPU memory %.1f MB%s (AMD SwapChain callback).",
			keyW, keyH, displayW, displayH, flags, g.stats.fgProvider, (unsigned long long)g.stats.fgProviderId,
			mem.totalUsageInBytes / 1048576.0, mrc == FFX_API_RETURN_OK ? "" : " (query failed)");
		return true;
	}

	ffxReturnCode_t NativeInterpolate(ffxDispatchDescFrameGeneration* params, void*)
	{
		auto* list = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
		const bool timed = list && std::exchange(nativePresent.timingRequested, false);
		if (timed) list->EndQuery(g.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, Slots * 2);
		const auto result = g.fx.Dispatch(&g.fg, &params->header);
		if (timed) {
			list->EndQuery(g.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, Slots * 2 + 1);
			list->ResolveQueryData(g.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, Slots * 2, 2,
				g.readback.Get(), Slots * 2 * sizeof(uint64_t));
			nativePresent.timingRecorded = true;
			nativePresent.timingValid = result == FFX_API_RETURN_OK && params->numGeneratedFrames;
		}
		if (result == FFX_API_RETURN_OK) nativePresent.generated += params->numGeneratedFrames;
		else nativePresent.error = result;
		return result;
	}

	void FgPrepare(const FrameData& fd, ID3D12GraphicsCommandList* l, uint32_t keyW, uint32_t keyH)
	{
		g.fgRetry.ObserveScene(fd.displayW, fd.displayH, keyW, keyH);
		if (!fd.fg || !g.fgDll || !nativePresent.chain || g.fgUnavailable)
		{
			if (fd.fg && g.fgDll && !nativePresent.chain) g_fgSkipReason = "AMD native SwapChain is unavailable";
			if (g.fg && WaitIdle("FG off")) DestroyFg();
			g.fgPending = false;
			if (!g.fgOffLogged) { g.fgOffLogged = true; Log("FG off."); }
			return;
		}
		if (!fg_camera::Valid(fd.cameraNear, fd.cameraFar, fd.fovV,
			fd.camPos, fd.camUp, fd.camRight, fd.camFwd)) {
			g_fgSkipReason = "Invalid camera parameters for FG";
			g.fgPending = g.fgHavePrevious = false;
			g.stats.fgSkipped++;
			return;
		}
		if (!g.fgRetry.Ready(GetTickCount64())) {
			g_fgSkipReason = g.fgRetry.failures >= FgRetry::MaxFailures ?
				"FG retry budget exhausted (waiting for resize/device change)" : "FG provider retry cooldown";
			g.fgPending = false;
			g.stats.fgSkipped++;
			return;
		}
		if (!EnsureFg(fd, keyW, keyH)) { g.fgPending = false; g.stats.fgSkipped++; return; }
		uint64_t id = ++g.fgFrameId;
		ffxConfigureDescFrameGeneration c = {};
		c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		c.swapChain = nativePresent.chain;
		c.frameGenerationEnabled = true;
		c.frameGenerationCallback = NativeInterpolate;
		c.allowAsyncWorkloads = false; // The timestamp fence is on the game graphics queue.
		const auto rect = g.fgContent;
		c.generationRect = { (int32_t)rect.x, (int32_t)rect.y, (int32_t)rect.width, (int32_t)rect.height };
		c.frameID = id;
		ffxConfigureDescFrameGenerationRegisterDistortionFieldResource distortion = {};
		const bool withDistortion=EnsureFgDistortion(l);
		if (withDistortion) {
			distortion.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE;
			distortion.distortionField=ffxApiGetResourceDX12(g.distortionTex.Get(),FFX_API_RESOURCE_STATE_COMPUTE_READ);
			c.header.pNext=&distortion.header;
		}
		ffxReturnCode_t rc = g.fx.Configure(&g.fg, &c.header);
		if (rc != FFX_API_RETURN_OK && withDistortion) {
			// Not all installed providers support this optional extension.
			// Preserve normal FG if the distortion resource is rejected.
			Log("FG provider rejected radial distortion field (%u); continuing with regular FG.",rc);
			g.distortionDisabled=true;
			c.header.pNext=nullptr;
			rc=g.fx.Configure(&g.fg,&c.header);
		}
		if (rc != FFX_API_RETURN_OK) {
			FgFailure(1, "configure", rc, id);
			BackoffFgFailure();
			g.fgPending = g.fgHavePrevious = false;
			g.stats.fgSkipped++;
			return;
		}
		nativePresent.enabled = true;
		ffxDispatchDescFrameGenerationPrepareV2 p = {};
		p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
		p.frameID = id;
		p.flags = c.flags;
		p.commandList = l;
		p.renderSize = { fd.renderW, fd.renderH };
		p.jitterOffset = { fd.jitterX, fd.jitterY };
		p.motionVectorScale = { fd.mvScaleX, fd.mvScaleY };
		p.frameTimeDelta = fd.frameTimeMs;
		p.reset = fd.reset != 0 || g.fgPending || !g.fgHavePrevious;
		p.cameraNear = fd.cameraNear;
		p.cameraFar = fd.cameraFar;
		p.cameraFovAngleVertical = fd.fovV;
		p.viewSpaceToMetersFactor = 1.0f;
		p.depth = ffxApiGetResourceDX12(g.depth.t12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		p.motionVectors = ffxApiGetResourceDX12(g.motion.t12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		memcpy(p.cameraPosition, fd.camPos, sizeof p.cameraPosition);
		memcpy(p.cameraUp, fd.camUp, sizeof p.cameraUp);
		memcpy(p.cameraRight, fd.camRight, sizeof p.cameraRight);
		memcpy(p.cameraForward, fd.camFwd, sizeof p.cameraForward);
		rc = g.fx.Dispatch(&g.fg, &p.header);
		if (rc != FFX_API_RETURN_OK) {
			FgFailure(2, "prepare", rc, id);
			BackoffFgFailure();
			g.fgPending = g.fgHavePrevious = false;
			g.stats.fgSkipped++;
			return;
		}
		g.stats.fgPrepared++;
		g.fgPending = true;
		if (g.fgOffLogged) { g.fgOffLogged = false; Log("FG prepared inputs for the game swap chain."); }
	}

	void Upscale(const FrameData& handed)
	{
		if (g_hudlessGen) {
			std::lock_guard<std::mutex> viewLock(g_viewLock);
			g_hudlessGen = 0;
		}
		if (!g.loaded || g.failed || !handed.color || !handed.depth || !handed.motion || !handed.output) return;
		FrameData fd = handed;
		float exposure = HasExternalExposure(g.exposureRequired, handed.exposure) ? handed.exposure : 0.0f;
		if (exposure > 0) fd.createFlags &= ~(uint32_t)FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
		float preExposure = 1.0f;
		// expStep only ever fed a log line that isn't in this build, it's dead weight now.
		float expStep = exposure > 0 ? floorf(log2f(exposure) * 4.0f + 0.5f) : -1000.0f;
		(void)expStep;

		ComPtr<ID3D11Device> dev;
		((ID3D11Resource*)fd.color)->GetDevice(&dev);
		if (dev.Get() != g.d11Base) dev = UnwrapReShade(dev.Get());
		if (g.d12 && dev.Get() != g.d11Base) {
			if (nativePresent.chain) return;
			Teardown("the game's D3D11 device changed");
		}
		if (!g.d12 && !InitDevices(dev.Get())) { Teardown("could not start"); Fail("the D3D12 side could not be made (above)."); return; }
		if (!EnsureShared(g.color, (ID3D11Texture2D*)fd.color, false, "color", true)
			|| !EnsureShared(g.depth, (ID3D11Texture2D*)fd.depth, false, "depth", false)
			|| !EnsureShared(g.motion, (ID3D11Texture2D*)fd.motion, false, "motion vectors", true)
			|| !EnsureShared(g.output, (ID3D11Texture2D*)fd.output, true, "output", true)) { Fail("shared textures (above)."); return; }
		if (!EnsureContext(fd)) { Fail("no FSR context (above)."); return; }
		if (!nativePresent.chain && !PresentHooked())
			InstallPresentHook(static_cast<ID3D11Resource*>(fd.color));

		int slot = (int)(g.submits % Slots);
		if (!WaitOut(g.allocFence[slot], FrameWaitMs, "command allocator reuse")) { Fail("the GPU stopped making progress (above)."); return; }
		ReadTimestamps(slot);
		ID3D12GraphicsCommandList* l = g.list[slot].Get();
		if (!Check(g.alloc[slot]->Reset(), "command allocator Reset") || !Check(l->Reset(g.alloc[slot].Get(), nullptr), "command list Reset"))
			{ Fail("command list (above)."); return; }

		if (!(fd.direct & DirectColor)) g.ctx->CopyResource(g.color.t11.Get(), (ID3D11Resource*)fd.color);
		g.ctx->CopyResource(g.depth.t11.Get(), (ID3D11Resource*)fd.depth);
		if (!(fd.direct & DirectMotion)) g.ctx->CopyResource(g.motion.t11.Get(), (ID3D11Resource*)fd.motion);
		if (fd.direct) g.stats.directFrames++;
		TimingMark(3);
		uint64_t ready = ++g.inValue;
		if (!Check(g.ctx->Signal(g.in11.Get(), ready), "D3D11 Signal")) { l->Close(); Fail("fence (above)."); return; }
		LARGE_INTEGER f0, f1, ff;
		QueryPerformanceCounter(&f0);
		g.ctx->Flush();
		QueryPerformanceCounter(&f1);
		QueryPerformanceFrequency(&ff);
		Average(g.stats.flushMs, (f1.QuadPart - f0.QuadPart) * 1000.0 / ff.QuadPart);

		D3D12_RESOURCE_BARRIER in[4], out[4];
		const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		Barrier(g.color.t12.Get(), D3D12_RESOURCE_STATE_COMMON, read, in[0]);
		Barrier(g.depth.t12.Get(), D3D12_RESOURCE_STATE_COMMON, read, in[1]);
		Barrier(g.motion.t12.Get(), D3D12_RESOURCE_STATE_COMMON, read, in[2]);
		Barrier(g.output.t12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, in[3]);
		l->ResourceBarrier(4, in);

		ffxDispatchDescUpscale d = {};
		d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
		d.commandList = l;
		d.color = ffxApiGetResourceDX12(g.color.t12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		d.depth = ffxApiGetResourceDX12(g.depth.t12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		d.motionVectors = ffxApiGetResourceDX12(g.motion.t12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		d.output = ffxApiGetResourceDX12(g.output.t12.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
		d.jitterOffset = { fd.jitterX, fd.jitterY };
		d.motionVectorScale = { fd.mvScaleX, fd.mvScaleY };
		d.renderSize = { fd.renderW, fd.renderH };
		d.upscaleSize = { fd.displayW, fd.displayH };
		d.enableSharpening = fd.sharpness > 0.0f;
		d.sharpness = fd.sharpness;
		d.frameTimeDelta = fd.frameTimeMs;
		d.preExposure = preExposure;
		if (exposure > 0)
		{
			if (!EnsureExposure(l, slot, exposure)) { l->Close(); Fail("exposure input (above)."); return; }
			d.exposure = ffxApiGetResourceDX12(g.expTex.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		}
		d.reset = fd.reset != 0;
		d.cameraNear = fd.cameraNear;
		d.cameraFar = fd.cameraFar;
		d.cameraFovAngleVertical = fd.fovV;
		d.viewSpaceToMetersFactor = 1.0f;
		d.flags = fd.dispatchFlags;
		l->EndQuery(g.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
		ffxReturnCode_t rc = g.fx.Dispatch(&g.fsr, &d.header);
		l->EndQuery(g.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
		g.stats.lastResult = (int32_t)rc;
		if (rc == FFX_API_RETURN_OK)
		{
			bool dynamic = (fd.createFlags & FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION) != 0;
			uint32_t keyW = dynamic ? (fd.maxRenderW > fd.renderW ? fd.maxRenderW : fd.renderW) : fd.renderW;
			uint32_t keyH = dynamic ? (fd.maxRenderH > fd.renderH ? fd.maxRenderH : fd.renderH) : fd.renderH;
			FgPrepare(fd, l, keyW, keyH);
		}
		else g.fgPending = g.fgHavePrevious = false;

		for (int i = 0; i < 4; i++)
		{
			out[i] = in[i];
			std::swap(out[i].Transition.StateBefore, out[i].Transition.StateAfter);
		}
		l->ResourceBarrier(4, out);
		l->ResolveQueryData(g.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, g.readback.Get(), slot * 2 * sizeof(uint64_t));
		if (!Check(l->Close(), "command list Close")) { Fail("command list (above)."); return; }

		if (!Check(g.queue->Wait(g.in12.Get(), ready), "queue Wait")) { Fail("fence (above)."); return; }
		ID3D12CommandList* lists[] = { l };
		g.queue->ExecuteCommandLists(1, lists);
		uint64_t done = ++g.outValue;
		if (!Check(g.queue->Signal(g.out12.Get(), done), "queue Signal")) { Fail("fence (above)."); return; }
		g.allocFence[slot] = done;
		g.slotTimed[slot] = true;
		g.submits++;

		if (!Check(g.ctx->Wait(g.out11.Get(), done), "D3D11 Wait")) { Fail("fence (above)."); return; }
		TimingMark(4);
		if (rc != FFX_API_RETURN_OK)
		{
			Log("ffxDispatch failed: %u (frame %llu).", rc, (unsigned long long)g.stats.frames);
			CheckRemoved();
			if (deviceLost || ++g.errors >= 10) Fail(deviceLost ? "device lost." : "ten failed dispatches.");
			return;
		}
		if (!(fd.direct & DirectOutput)) g.ctx->CopyResource((ID3D11Resource*)fd.output, g.output.t11.Get());
		if (fd.fg && fd.hudless == fd.output && g.output.srv) {
			std::lock_guard<std::mutex> viewLock(g_viewLock);
			g_hudlessGen = g_viewGen;
			g.stats.fgHudless++;
		}
		g.stats.frames++;
	}

	void Release()
	{
		if (!g.d12) return;
		if (!WaitIdle("release")) { Teardown("the queue was not idle at release"); return; }
		ReleaseShared();
		Log("Bridge released its shared textures (device and FSR context kept).");
	}

	const int TCodes = 48;
	// Does nothing in this build. The calls stay cos stripping them out wasn't worth touching the render event for.
	void TimingMark(int) {}

	void RunRenderEvent(int eventId, void* data)
	{
		__try
		{
			if (g.teardownDue) {
				g.teardownDue = false;
				if (!nativePresent.chain) Teardown("after an exception");
			}
			if (eventId == 1 && data)
			{
				LARGE_INTEGER t0, t1, f;
				QueryPerformanceCounter(&t0);
				uint64_t before = g.stats.frames;
				made = false;
				TimingMark(1);
				Upscale(*(const FrameData*)data);
				TimingMark(2);
				QueryPerformanceCounter(&t1);
				QueryPerformanceFrequency(&f);
				if (g.stats.frames != before && !made) Average(g.stats.cpuMs, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
			}
			else if (eventId == 2) Release();
			else if (eventId == 3) g.stats.presented++;
			else if (eventId >= 100 && eventId < 100 + TCodes) TimingMark(eventId - 100);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			DWORD code = GetExceptionCode();
			if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_STACK_OVERFLOW) g.fatal = true;
			g.teardownDue = true;
			g.errors = 0;
			g.stats.failures++;
			g.failed = true;
			Log("FSR stopped: exception 0x%08X on the render thread%s.", (unsigned)code, g.fatal ? " (a memory fault: off until the game restarts)" : "");
		}
		Publish();
	}
}

void __stdcall OnRenderEvent(int eventId, void* data)
{
	std::lock_guard<std::mutex> lock(g_renderLock);
	RunRenderEvent(eventId, data);
}

void BridgeLog(const char* fmt, ...)
{
	char buf[1024];
	va_list a;
	va_start(a, fmt);
	vsnprintf(buf, sizeof buf, fmt, a);
	va_end(a);
	Log("%s", buf);
}

HRESULT CreateNativeSwapchain(IDXGIFactory2* factory, ID3D11Device* device, HWND window, DXGI_SWAP_CHAIN_DESC1& desc,
	const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, NativeSwapchainState& state)
{
	std::lock_guard<std::mutex> lock(g_renderLock);
	const auto nativeDevice = UnwrapReShade(device);
	device = nativeDevice.Get();
	if (!g.fgDll || nativePresent.chain || (g.d12 && g.d11Base != device)) return DXGI_ERROR_UNSUPPORTED;
	if (!g.d12 && !InitDevices(device)) {
		Teardown("native swap chain startup failed");
		return DXGI_ERROR_UNSUPPORTED;
	}
	ComPtr<ID3D11Multithread> multithread;
	if ((device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) || FAILED(g.ctx.As(&multithread)))
		return DXGI_ERROR_UNSUPPORTED;

	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	HRESULT hr = g.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
	if (SUCCEEDED(hr)) hr = g.d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		allocator.Get(), nullptr, IID_PPV_ARGS(&list));
	if (SUCCEEDED(hr)) hr = list->Close();
	if (FAILED(hr)) return hr;

	ffxCreateContextDescFrameGenerationSwapChainVersionDX12 version = {};
	version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
	version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
	ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create = {};
	create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
	create.header.pNext = &version.header;
	create.swapchain = &state.swapchain;
	create.gameQueue = g.queue.Get();
	const auto nativeFactory = UnwrapReShade(factory);
	create.dxgiFactory = nativeFactory.Get();
	create.hwnd = window;
	create.desc = &desc;
	DXGI_SWAP_CHAIN_FULLSCREEN_DESC full = {};
	if (fullscreen) { full = *fullscreen; create.fullscreenDesc = &full; }
	const auto result = g.fx.CreateContext(&state.context, &create.header, nullptr);
	if (result != FFX_API_RETURN_OK || !state.swapchain) {
		Log("AMD native SwapChain startup failed: FFX error %u.", result);
		if (state.context) g.fx.DestroyContext(&state.context, nullptr);
		if (state.swapchain) state.swapchain->Release();
		state = {};
		return DXGI_ERROR_UNSUPPORTED;
	}
	DXGI_SWAP_CHAIN_DESC1 actual = {};
	if (SUCCEEDED(state.swapchain->GetDesc1(&actual)))
		g.fgRetry.ObserveBackbuffer(actual.Width, actual.Height);
	nativePresent.chain = state.swapchain;
	nativePresent.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
	nativePresent.swapchainContext = state.context;
	nativePresent.allocator = std::move(allocator);
	nativePresent.list = std::move(list);
	nativePresent.fence = 0;
	nativePresent.timingFence = 0;
	nativePresent.timingRequested = nativePresent.timingRecorded = nativePresent.timingValid = nativePresent.timingDisabled = false;
	nativePresent.generated = 0;
	nativePresent.error = FFX_API_RETURN_OK;
	nativePresent.enabled = false;
	g_dafpWindow.store(window);
	g_dafpSwapchainMajor.store(0);
	g_dafpLastPresentTick.store(0);
	g_dafpLastGeneratedTick.store(0);
	g_nativeSwapchainActive = true;
	multithread->SetMultithreadProtected(TRUE);
	ffxQueryGetProviderVersion provider = {};
	provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
	g.fx.Query(&state.context, &provider.header);
	g_dafpSwapchainMajor.store(DafpSwapchainMajor(provider.versionName));
	Log("Unity AMD native SwapChain active: %ux%u, provider %s, flags 0x%x.",
		desc.Width, desc.Height, provider.versionName ? provider.versionName : "unknown", desc.Flags);
	return S_OK;
}

void DestroyNativeSwapchain(NativeSwapchainState& state)
{
	if (!state.context) return;
	std::lock_guard<std::mutex> lock(g_renderLock);
	if (nativePresent.chain == state.swapchain) {
		WaitIdle("native swap-chain shutdown", true);
		DestroyFg();
		nativePresent.chain = nullptr;
		g_nativeSwapchainActive = false;
		g_dafpWindow.store(nullptr);
		g_dafpSwapchainMajor.store(0);
		g_dafpLastPresentTick.store(0);
		g_dafpLastGeneratedTick.store(0);
		nativePresent.swapchainContext = nullptr;
		nativePresent.list.Reset();
		nativePresent.allocator.Reset();
	}
	g.fx.DestroyContext(&state.context, nullptr);
	state.swapchain->Release();
	state = {};
}

HRESULT ResizeNativeSwapchain(NativeSwapchainState& state, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
	std::lock_guard<std::mutex> lock(g_renderLock);
	if (!WaitIdle("native swap-chain resize")) return DXGI_ERROR_DEVICE_REMOVED;
	DestroyFg();
	const HRESULT hr = state.swapchain->ResizeBuffers(count, width, height, format, flags);
	DXGI_SWAP_CHAIN_DESC1 desc = {};
	if (SUCCEEDED(hr) && SUCCEEDED(state.swapchain->GetDesc1(&desc))) {
		g.fgRetry.ObserveBackbuffer(desc.Width, desc.Height);
		Log("AMD native SwapChain resized: %ux%u, format %u.", desc.Width, desc.Height, desc.Format);
	}
	return hr;
}

HRESULT SetNativeSwapchainColorSpace(NativeSwapchainState& state, DXGI_COLOR_SPACE_TYPE color)
{
	std::lock_guard<std::mutex> lock(g_renderLock);
	const HRESULT hr = state.swapchain->SetColorSpace1(color);
	if (SUCCEEDED(hr) && nativePresent.colorSpace != color) {
		nativePresent.colorSpace = color;
		g.fgPending = g.fgHavePrevious = false;
		Log("AMD native SwapChain color space: %u.", color);
	}
	return hr;
}

HRESULT PresentNativeSwapchain(NativeSwapchainState& state, ID3D11Texture2D* source, UINT sync, UINT flags,
	const DXGI_PRESENT_PARAMETERS* parameters)
{
	LARGE_INTEGER appEntry{};
	QueryPerformanceCounter(&appEntry);
	std::lock_guard<std::mutex> lock(g_renderLock);
	const auto present = [&] {
		return parameters ? state.swapchain->Present1(sync, flags, parameters) : state.swapchain->Present(sync, flags);
	};
	if (flags & DXGI_PRESENT_TEST) return present();
	if (deviceLost || !g.ctx) return DXGI_ERROR_DEVICE_REMOVED;
	if (nativePresent.enabled && (!g.fgPending || g.failed ||
		(parameters && (parameters->DirtyRectsCount || parameters->pScrollRect || parameters->pScrollOffset)))) {
		ffxConfigureDescFrameGeneration c = {};
		c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		c.swapChain = state.swapchain;
		c.frameID = g.fgFrameId;
		if (g.fx.Configure(&g.fg, &c.header) != FFX_API_RETURN_OK) return E_FAIL;
		nativePresent.enabled = false;
		g.fgHavePrevious = false;
	}
	g.fgPending = false;
	if (!WaitOut(nativePresent.fence, FrameWaitMs, "native swap-chain command reuse")) return DXGI_ERROR_DEVICE_REMOVED;
	// An unsupported texture layout or allocation failure is not a removed GPU.
	if (!EnsureShared(g.present, source, false, "native SwapChain colour", false)) return E_FAIL;
	ComPtr<ID3D12Resource> dest;
	HRESULT hr = state.swapchain->GetBuffer(state.swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&dest));
	if (FAILED(hr)) return hr;
	const auto desc = dest->GetDesc();
	if (Typed(desc.Format) != g.present.fmt || desc.Width != g.present.w || desc.Height != g.present.h)
		return DXGI_ERROR_INVALID_CALL;
	auto* allocator = nativePresent.allocator.Get();
	auto* list = nativePresent.list.Get();
	hr = allocator->Reset();
	if (SUCCEEDED(hr)) hr = list->Reset(allocator, nullptr);
	if (FAILED(hr)) return hr;
	g.ctx->CopyResource(g.present.t11.Get(), source);
	const auto ready = ++g.inValue;
	hr = g.ctx->Signal(g.in11.Get(), ready);
	if (FAILED(hr)) { list->Close(); return hr; }
	g.ctx->Flush();
	D3D12_RESOURCE_BARRIER copies[2];
	Barrier(g.present.t12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE, copies[0]);
	Barrier(dest.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST, copies[1]);
	list->ResourceBarrier(2, copies);
	list->CopyResource(dest.Get(), g.present.t12.Get());
	for (auto& barrier : copies) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
	list->ResourceBarrier(2, copies);
	hr = list->Close();
	if (SUCCEEDED(hr)) hr = g.queue->Wait(g.in12.Get(), ready);
	if (FAILED(hr)) return hr;
	ID3D12CommandList* lists[] = {list};
	g.queue->ExecuteCommandLists(1, lists);
	const auto done = ++g.outValue;
	hr = g.queue->Signal(g.out12.Get(), done);
	if (SUCCEEDED(hr)) hr = g.ctx->Wait(g.out11.Get(), done);
	if (FAILED(hr)) return hr;
	nativePresent.fence = done;
	const auto completed = nativePresent.timingFence ? g.out12->GetCompletedValue() : 0;
	if (nativePresent.timingFence && completed != UINT64_MAX && completed >= nativePresent.timingFence) {
		const auto elapsed = nativePresent.timingValid ? ReadGpuTime(Slots) : 0;
		if (elapsed > 0) {
			Average(g.stats.fgGpuMs, elapsed);
			static bool reported = false;
			if (!reported && g.stats.fgIntervalMs > 0) {
				reported = true;
				Log("AMD SwapChain FG timing: GPU %.2f ms, source interval %.2f ms.",
					g.stats.fgGpuMs, g.stats.fgIntervalMs);
			}
		}
		nativePresent.timingFence = 0;
	}
	nativePresent.timingRequested = nativePresent.enabled && !nativePresent.timingDisabled &&
		!nativePresent.timingFence && g.stats.presented % 30 == 0;
	nativePresent.timingRecorded = false;
	LARGE_INTEGER entry, frequency;
	QueryPerformanceCounter(&entry);
	QueryPerformanceFrequency(&frequency);
	hr = present();
	if (nativePresent.timingRecorded) {
		const auto timingDone = ++g.outValue;
		if (SUCCEEDED(g.queue->Signal(g.out12.Get(), timingDone))) nativePresent.timingFence = timingDone;
		else nativePresent.timingDisabled = true;
	}
	const auto frames = nativePresent.generated.exchange(0);
	const auto failure = nativePresent.error.exchange(FFX_API_RETURN_OK);
	if (frames || failure != FFX_API_RETURN_OK) g.stats.fgLastResult = static_cast<int32_t>(failure);
	if (failure != FFX_API_RETURN_OK) {
		FgFailure(3, "native SwapChain callback", failure, g.fgFrameId);
		BackoffFgFailure();
		g.stats.fgSkipped++;
	}
	g.fgHavePrevious = SUCCEEDED(hr) && failure == FFX_API_RETURN_OK && nativePresent.enabled;
	if (SUCCEEDED(hr)) {
		if (hr == S_OK) {
			g_dafpSyncInterval.store(sync);
			g_dafpLastPresentTick.store(GetTickCount64());
			CountPresentFrames(1, frames);
			RecordGamePresentQpc(static_cast<uint64_t>(appEntry.QuadPart));
			if (nativePresent.enabled) ObserveFgInterval(entry.QuadPart, frequency.QuadPart);
		}
		if (frames) {
			if (failure == FFX_API_RETURN_OK) g_dafpLastGeneratedTick.store(GetTickCount64());
			if (g.stats.fgGenerated == 0) Log("First AMD SwapChain interpolation: SyncInterval=%u, flags=0x%x.", sync, flags);
			g.stats.fgGenerated += frames;
			g.stats.fgReal++;
		}
		if (frames && failure == FFX_API_RETURN_OK && nativePresent.enabled) {
			FgRecovered();
			g.fgRetry.Recovered();
			g_fgSkipReason = nullptr;
		}
		g.stats.presented++;
		Publish();
	}
	return hr;
}

int FSR4_SetFgRadialDistortion(float k1, float k2, float centerX, float centerY)
{
	// Called during plugin activation, before any render thread is scheduled.
	if (g.submits || !std::isfinite(k1) || !std::isfinite(k2) ||
		!std::isfinite(centerX) || !std::isfinite(centerY) ||
		fabsf(k1) > 4 || fabsf(k2) > 4 ||
		centerX < 0 || centerX > 1 || centerY < 0 || centerY > 1) return 0;
	g.distortionK1=k1; g.distortionK2=k2;
	g.distortionCenterX=centerX; g.distortionCenterY=centerY;
	return 1;
}

int FSR4_Load(const wchar_t* dir)
{
	if (g.loaded) return 1;
	std::wstring d(dir);
	if (!d.empty() && d.back() != L'\\') d += L'\\';
	g.upscalerDll = LoadLibraryExW((d + L"amd_fidelityfx_upscaler_dx12.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!g.upscalerDll) { Log("Could not load amd_fidelityfx_upscaler_dx12.dll: error %lu.", GetLastError()); return 0; }
	g.loaderDll = LoadLibraryExW((d + L"amd_fidelityfx_loader_dx12.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!g.loaderDll) { Log("Could not load amd_fidelityfx_loader_dx12.dll: error %lu.", GetLastError()); return 0; }
	ffxLoadFunctions(&g.fx, g.loaderDll);
	if (!g.fx.CreateContext || !g.fx.Dispatch || !g.fx.Query || !g.fx.DestroyContext || !g.fx.Configure) { Log("The FSR loader lacks its entry points."); return 0; }
	g.loaded = true;
	return 1;
}

int FSR4_SetProviderVersion(const char* version)
{
	if (g.providersListed || g.fsr || !version) return 0;
	if (!*version) { g.wantProvider = "4."; return 1; }
	const FsrVersion parsed = ParseFsrProviderVersion(version);
	if (!parsed.valid || parsed.major != 4) return 0;
	g.wantProvider = version;
	return 1;
}

int FSR4_LoadFrameGeneration(const wchar_t* dir)
{
	if (!g.fgDll) {
		std::wstring d(dir);
		if (!d.empty() && d.back() != L'\\') d += L'\\';
		g.fgDll = LoadLibraryExW((d + L"amd_fidelityfx_framegeneration_dx12.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!g.fgDll) {
			g_fgSkipReason = "AMD frame generation runtime DLL could not load";
			Log("Could not load amd_fidelityfx_framegeneration_dx12.dll: error %lu. Continuing with upscaling.", GetLastError());
			return 0;
		}
	}
	return 1;
}

void FSR4_SetFgViewportOverride(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	std::lock_guard<std::mutex> lock(g_renderLock);
	g.fgViewportOverride = {x,y,width,height};
	g.fgViewportOverrideEnabled = width && height;
}

void* FSR4_GetRenderEventFunc() { return (void*)&OnRenderEvent; }

int FSR4_PollLog(char* buf, int cap)
{
	std::lock_guard<std::mutex> l(g_logLock);
	if (cap <= 0) return 0;
	if (g_log.empty())
	{
		if (g_logDropped == g_logDroppedReported) return 0;
		int n = snprintf(buf, cap, "(%llu bridge log lines lost: the log was not read in time.)", (unsigned long long)(g_logDropped - g_logDroppedReported));
		g_logDroppedReported = g_logDropped;
		return n < 0 ? 0 : min(n, cap - 1);
	}
	int n = (int)min((size_t)(cap - 1), g_log.front().size());
	memcpy(buf, g_log.front().data(), n);
	buf[n] = 0;
	g_log.pop_front();
	return n;
}

const char* FSR4_GetLastFgSkipReason() { return g_fgSkipReason.load(); }
extern "C" bool FSR4_UsesNativeSwapchain() { return g_nativeSwapchainActive.load(); }

void FSR4_GetDafpDiagnostics(DafpDiagnostics* out)
{
	if (!out) return;
	DafpDiagnostics snapshot{};
	snapshot.nativeSwapchain = g_nativeSwapchainActive.load();
	if (snapshot.nativeSwapchain) {
		snapshot.window = g_dafpWindow.load();
		snapshot.swapchainMajor = g_dafpSwapchainMajor.load();
		snapshot.syncInterval = g_dafpSyncInterval.load();
		const uint64_t now = GetTickCount64();
		const uint64_t present = g_dafpLastPresentTick.load();
		const uint64_t generated = g_dafpLastGeneratedTick.load();
		snapshot.presentRecent = present && now >= present && now - present < 2500;
		snapshot.generatedRecent = generated && now >= generated && now - generated < 2500;
	}
	*out = snapshot;
}

void FSR4_GetStats(Stats* out)
{
	if (!out) return;
	{
		std::lock_guard<std::mutex> l(g_statsLock);
		*out = g_published;
	}
	out->failed = g.failed ? 1 : 0;
	std::lock_guard<std::mutex> l(g_logLock);
	out->logDropped = g_logDropped;
}

int FSR4_LayoutField(int which, int i, const char** name, uint32_t* offset)
{
	if (!name || !offset || which < 0 || which > 1) return 0;
	const LayoutField* f = which == 0 ? kFrameFields : kStatsFields;
	int n = which == 0 ? (int)(sizeof kFrameFields / sizeof *kFrameFields) : (int)(sizeof kStatsFields / sizeof *kStatsFields);
	if (i == -1) { *name = which == 0 ? "FrameData" : "Stats"; *offset = which == 0 ? (uint32_t)sizeof(FrameData) : (uint32_t)sizeof(Stats); return 1; }
	if (i < 0 || i >= n) return 0;
	*name = f[i].name;
	*offset = f[i].offset;
	return 1;
}

int FSR4_Failed() { return g.fatal ? 2 : g.failed ? 1 : 0; }

int FSR4_ExposureWanted() { return g.exposureRequired ? 1 : 0; }

uint64_t FSR4_GetViews(void** color, void** motion, void** output)
{
	std::lock_guard<std::mutex> l(g_viewLock);
	*color = g.color.srv.Get(); *motion = g.motion.srv.Get(); *output = g.output.srv.Get();
	return *color && *motion && *output ? g_viewGen : 0;
}

uint64_t FSR4_GetHudlessView(void** hudless)
{
	if (!hudless) return 0;
	std::lock_guard<std::mutex> l(g_viewLock);
	*hudless = g_hudlessGen && g_hudlessGen == g_viewGen ? g.output.srv.Get() : nullptr;
	return *hudless ? g_viewGen : 0;
}

void FSR4_ViewsInUse(uint64_t gen)
{
	std::lock_guard<std::mutex> l(g_viewLock);
	g_viewTicks++;
	for (size_t i = 0; i < g_retired.size();)
	{
		Retired& r = g_retired[i];
		if (gen != 0 && r.gen >= gen) { r.unusedSince = 0; i++; continue; }
		if (!r.unusedSince) r.unusedSince = g_viewTicks;
		if (g_viewTicks - r.unusedSince >= 3) g_retired.erase(g_retired.begin() + i);
		else i++;
	}
}

void FSR4_ClearFailure() { if (!g.fatal) g.failed = false; }
