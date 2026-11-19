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
#include <vector>

#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "dx12/ffx_api_dx12.h"
#include "ffx_upscale.h"
#include "ffx_framegeneration.h"
#include "../../src/ExposurePolicy.h"
#include <timeapi.h>

using Microsoft::WRL::ComPtr;

#define EXPORT extern "C" __declspec(dllexport)

void InstallPresentHook(ID3D11Resource* anchor);
bool IsPresentWindow(HWND window);
bool PresentHooked();
uint64_t PresentsSeen();
using PresentCall = HRESULT (*)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

struct FrameData
{
	void* color;
	void* depth;
	void* motion;
	void* output;
	uint32_t renderW, renderH, displayW, displayH;
	float jitterX, jitterY, mvScaleX, mvScaleY;
	float frameTimeMs, cameraNear, cameraFar, fovV;
	float sharpness;
	int32_t reset;
	uint32_t createFlags, dispatchFlags;
	float exposure;
	uint32_t direct;
	uint32_t maxRenderW, maxRenderH;
	void* hudless;
	uint32_t fg;
	float camPos[3], camUp[3], camRight[3], camFwd[3];
};

struct Stats
{
	uint64_t frames;
	uint64_t providerId;
	int32_t lastResult;
	int32_t failed;
	char provider[64];
	char adapter[128];
	uint64_t presented;
	float cpuMs;
	float gpuMs;
	uint64_t fsrMemory;
	uint64_t logDropped;
	uint32_t bridgeStarts;
	uint32_t contextStarts;
	int32_t removedReason;
	uint32_t failures;
	// gpuPeriodMs down to markMs stay at zero in this build, nothing fills them. Left in so the layout still matches Native.cs.
	float gpuPeriodMs, gpuPeriodMin, gpuPeriodMax;
	float gpuBusyMs, gpuBusyMin, gpuBusyMax;
	uint64_t gpuFrames;
	uint64_t gpuDisjoint;
	uint64_t gpuDropped;
	float markMs[48];
	float flushMs;
	uint32_t exposureWanted;
	uint64_t directFrames;
	char fgProvider[64];
	uint64_t fgProviderId, fgPrepared, fgGenerated, fgReal, fgSkipped, fgMemory;
	int32_t fgLastResult;
	float fgGpuMs, fgPaceMs, fgIntervalMs;
	float fgHookMs;
	float fgRefreshHz;
	uint64_t fgNotNeeded;
	uint64_t presentsHooked;
	uint64_t fgHudless;
};

static_assert(sizeof(void*) == 8, "64-bit only");
static_assert(sizeof(FrameData) == 176, "FrameData changed: update Native.cs, kFrameFields and this size");
static_assert(sizeof(Stats) == 680, "Stats changed: update Native.cs, kStatsFields and this size");

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
		char buf[1024];
		WideCharToMultiByte(CP_UTF8, 0, message, -1, buf, sizeof buf, nullptr, nullptr);
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
	uint64_t g_viewGen = 0;
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

	struct ImageRect
	{
		UINT x, y, width, height;
	};

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
		bool slotFg[Slots] = {};
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
		float expValue = 0;
		bool exposureRequired = false;
		uint64_t providerGen = 0;
		std::string wantProvider = "4."; // AMD's provider names are numeric; never silently select FSR 3.
		uint64_t overrideId = 0;
		bool providersListed = false;
		uint32_t ctxDisplayW = 0, ctxDisplayH = 0, ctxRenderW = 0, ctxRenderH = 0, ctxFlags = 0;
		int errors = 0;
		Stats stats = {};

		ffxContext fg = nullptr;
		uint32_t fgDisplayW = 0, fgDisplayH = 0, fgRenderW = 0, fgRenderH = 0, fgFlags = 0;
		bool fgProvidersListed = false;
		bool fgUnavailable = false;
		ImageRect fgContent{};
		UINT fgSceneW = 0, fgSceneH = 0;
		Shared present, gen, fgColor, fgOutput;
		ComPtr<ID3D12RootSignature> scaleRs;
		ComPtr<ID3D12PipelineState> scalePso;
		ComPtr<ID3D12DescriptorHeap> scaleHeap;
		UINT scaleInc = 0;
		DWORD renderThread = 0;
		bool fgCrossThread = false;
		uint64_t fgFrameId = 0;
		bool fgPending = false;
		bool fgPendingReset = false;
		bool fgHavePrevious = false;
		int fgErrors = 0;
		LONGLONG fgLastEntry = 0;
		uint64_t fgRefreshFrames = 0;
		double fgIntervalTicks = 0;
		bool fgOffLogged = true;
	};
	// The host never unloads plugins. COM teardown under the DLL loader lock can fail-fast in the driver.
	State& g = *new State;
	std::mutex g_renderLock;

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
		g.gen.Release();
		g.fgColor.Release();
		g.fgOutput.Release();
	}

	void DestroyFg()
	{
		if (g.fg && g.fx.DestroyContext)
		{
			ffxConfigureDescFrameGeneration c = {};
			c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
			c.frameGenerationEnabled = false;
			c.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
			c.frameID = g.fgFrameId;
			g.fx.Configure(&g.fg, &c.header);
			g.fx.DestroyContext(&g.fg, nullptr);
			Log("FG context destroyed (%llu frames prepared, %llu generated frames presented).",
				(unsigned long long)g.stats.fgPrepared, (unsigned long long)g.stats.fgGenerated);
		}
		g.fg = nullptr;
		g.fgDisplayW = g.fgDisplayH = g.fgRenderW = g.fgRenderH = g.fgFlags = 0;
		g.fgPending = g.fgHavePrevious = false;
		g.fgLastEntry = 0;
		g.fgIntervalTicks = 0;
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
			g.scalePso.Reset(); g.scaleRs.Reset(); g.scaleHeap.Reset();
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
			for (Shared* s : { &g.color, &g.depth, &g.motion, &g.output, &g.present, &g.gen, &g.fgColor, &g.fgOutput }) { s->t12.Detach(); s->Release(); }
			for (int i = 0; i < Slots; i++) { g.list[i].Detach(); g.alloc[i].Detach(); }
			g.timestamps.Detach();
			g.readback.Detach();
			g.expTex.Detach();
			g.expUpload.Detach();
			g.scalePso.Detach(); g.scaleRs.Detach(); g.scaleHeap.Detach();
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
		g.fgCrossThread = false;
		g.fgContent = {};
		g.inValue = g.outValue = 0;
		g.expValue = 0;
		g.providersListed = false;
		g.overrideId = 0;
		g.exposureRequired = false;
		deviceLost = false;
		Log("Bridge shut down (%s)%s.", why, idle ? "" : "; its GPU objects abandoned, the queue was not idle");
	}

	bool InitDevices(ID3D11Resource* any)
	{
		ComPtr<ID3D11Device> dev;
		any->GetDevice(&dev);
		g.d11Base = dev.Get();
		if (!Check(dev.As(&g.d11), "ID3D11Device5 (Windows 10 1703 or later)")) return false;
		ComPtr<ID3D11DeviceContext> ic;
		dev->GetImmediateContext(&ic);
		if (!Check(ic.As(&g.ctx), "ID3D11DeviceContext4")) return false;

		ComPtr<IDXGIDevice> xd;
		ComPtr<IDXGIAdapter> ad;
		DXGI_ADAPTER_DESC ds = {};
		if (!Check(dev.As(&xd), "IDXGIDevice") || !Check(xd->GetAdapter(&ad), "GetAdapter") || !Check(ad->GetDesc(&ds), "GetDesc")) return false;
		WideCharToMultiByte(CP_UTF8, 0, ds.Description, -1, g.stats.adapter, sizeof g.stats.adapter, nullptr, nullptr);

		ComPtr<IDXGIFactory4> factory;
		ComPtr<IDXGIAdapter1> ad12;
		if (!Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2")) return false;
		if (!Check(factory->EnumAdapterByLuid(ds.AdapterLuid, IID_PPV_ARGS(&ad12)), "EnumAdapterByLuid")) return false;
		if (!Check(D3D12CreateDevice(ad12.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.d12)), "D3D12CreateDevice")) return false;

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
		qh.Count = 2 * Slots;
		if (!Check(g.d12->CreateQueryHeap(&qh, IID_PPV_ARGS(&g.timestamps)), "CreateQueryHeap")) return false;
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = 2 * Slots * sizeof(uint64_t);
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
		if (s.t11 && s.w == sd.Width && s.h == sd.Height && s.fmt == fmt) return true;
		if (sd.SampleDesc.Count != 1) { Log("%s: multisampled textures are not supported.", name); return false; }
		if (sd.MipLevels != 1 || sd.ArraySize != 1) { Log("%s: %u mips, %u slices: CopyResource needs exactly one of each.", name, sd.MipLevels, sd.ArraySize); return false; }
		if (Typeless(fmt)) { Log("%s: no typed format for game texture format %d.", name, (int)sd.Format); return false; }
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
		for (uint64_t i = 0; i < count; i++)
		{
			char item[96];
			snprintf(item, sizeof item, "%s'%s' (id 0x%llX)", i ? ", " : "", names[i] ? names[i] : "?", (unsigned long long)ids[i]);
			list += item;
			if (!g.overrideId && !g.wantProvider.empty() && names[i] && strncmp(names[i], g.wantProvider.c_str(), g.wantProvider.size()) == 0)
				g.overrideId = ids[i];
		}
		Log("FSR providers on this GPU (%llu): %s.", (unsigned long long)count, list.c_str());
		if (g.wantProvider.empty()) return true;
		if (!g.overrideId) { Log("FSR provider '%s' asked for: not offered here.", g.wantProvider.c_str()); return false; }
		Log("FSR provider forced: '%s' (id 0x%llX).", g.wantProvider.c_str(), (unsigned long long)g.overrideId);
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
				req && !(fd.exposure > 0) ? "no game exposure is captured, attempting SDK auto exposure" : "using the available exposure mode");
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

	typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
	static const char kScaleHlsl[] = R"(
Texture2D<float4> src : register(t0);
SamplerState linearClamp : register(s0);
RWTexture2D<float4> dst : register(u0);
cbuffer Rectangles : register(b0)
{
	uint4 sourceRect;
	uint4 destinationRect;
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= destinationRect.zw)) return;
	uint w, h;
	src.GetDimensions(w, h);
	float2 origin = float2(sourceRect.xy);
	float2 size = float2(sourceRect.zw);
	float2 samplePosition = origin + (float2(id.xy) + 0.5) * size / float2(destinationRect.zw);
	samplePosition = clamp(samplePosition, origin + 0.5, origin + size - 0.5);
	dst[destinationRect.xy + id.xy] = src.SampleLevel(linearClamp, samplePosition / float2(w, h), 0);
}
)";

	bool EnsureScale()
	{
		if (g.scaleHeap) return true;
		static HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		auto compile = compiler ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
		if (!compile) { Log("FG scaling: d3dcompiler_47.dll unavailable."); return false; }
		ComPtr<ID3DBlob> shader, errors;
		HRESULT hr = compile(kScaleHlsl, sizeof kScaleHlsl - 1, "FG scale", nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors);
		if (FAILED(hr)) {
			Log("FG scaling: shader compile failed: 0x%08X %s.", (unsigned)hr, errors ? (const char*)errors->GetBufferPointer() : "");
			return false;
		}
		const D3D12_DESCRIPTOR_RANGE ranges[]{
			{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
			{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}
		};
		D3D12_ROOT_PARAMETER params[2]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[0].DescriptorTable = {_countof(ranges), ranges};
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[1].Constants.Num32BitValues = 8;
		D3D12_STATIC_SAMPLER_DESC sampler{};
		sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.MaxAnisotropy = 1;
		sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		const D3D12_ROOT_SIGNATURE_DESC desc{_countof(params), params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
		ComPtr<ID3DBlob> signature;
		if (!Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, nullptr), "FG scale root signature serialize")) return false;
		if (!Check(g.d12->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&g.scaleRs)), "FG scale root signature")) return false;
		D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
		pipeline.pRootSignature = g.scaleRs.Get();
		pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
		if (!Check(g.d12->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&g.scalePso)), "FG scale pipeline")) return false;
		D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4 * Slots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
		if (!Check(g.d12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g.scaleHeap)), "FG scale descriptor heap")) return false;
		g.scaleInc = g.d12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		return true;
	}

	void ScaleFrame(ID3D12GraphicsCommandList* list, const Shared& src, const Shared& dst,
		ImageRect sourceRect, ImageRect destinationRect, UINT descriptor)
	{
		D3D12_RESOURCE_BARRIER barriers[2];
		Barrier(src.t12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, barriers[0]);
		Barrier(dst.t12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, barriers[1]);
		list->ResourceBarrier(_countof(barriers), barriers);

		auto cpu = g.scaleHeap->GetCPUDescriptorHandleForHeapStart();
		auto gpu = g.scaleHeap->GetGPUDescriptorHandleForHeapStart();
		cpu.ptr += SIZE_T(descriptor) * g.scaleInc;
		gpu.ptr += UINT64(descriptor) * g.scaleInc;
		g.d12->CreateShaderResourceView(src.t12.Get(), nullptr, cpu);
		cpu.ptr += g.scaleInc;
		g.d12->CreateUnorderedAccessView(dst.t12.Get(), nullptr, nullptr, cpu);
		ID3D12DescriptorHeap* heaps[]{g.scaleHeap.Get()};
		list->SetDescriptorHeaps(_countof(heaps), heaps);
		list->SetComputeRootSignature(g.scaleRs.Get());
		list->SetPipelineState(g.scalePso.Get());
		list->SetComputeRootDescriptorTable(0, gpu);
		const UINT rectangles[]{sourceRect.x, sourceRect.y, sourceRect.width, sourceRect.height,
			destinationRect.x, destinationRect.y, destinationRect.width, destinationRect.height};
		list->SetComputeRoot32BitConstants(1, _countof(rectangles), rectangles, 0);
		list->Dispatch((destinationRect.width + 7) / 8, (destinationRect.height + 7) / 8, 1);

		for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		list->ResourceBarrier(_countof(barriers), barriers);
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

	void ReadTimestamps(int slot)
	{
		if (!g.slotTimed[slot]) return;
		g.slotTimed[slot] = false;
		D3D12_RANGE r = { slot * 2 * sizeof(uint64_t), (slot * 2 + 2) * sizeof(uint64_t) };
		void* p = nullptr;
		if (FAILED(g.readback->Map(0, &r, &p))) return;
		const uint64_t* t = (const uint64_t*)((const char*)p + r.Begin);
		if (t[1] > t[0]) Average(g.slotFg[slot] ? g.stats.fgGpuMs : g.stats.gpuMs, (t[1] - t[0]) * g.tickMs);
		g.slotFg[slot] = false;
		D3D12_RANGE none = { 0, 0 };
		g.readback->Unmap(0, &none);
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

	bool MatchingAspectRatio(UINT w, UINT h, UINT sceneW, UINT sceneH)
	{
		if (!w || !h || !sceneW || !sceneH) return false;
		double ratio = double(w) * sceneH / (double(h) * sceneW);
		return ratio >= 0.99 && ratio <= 1.01;
	}

	ImageRect FitScene(UINT w, UINT h, UINT sceneW, UINT sceneH)
	{
		if (MatchingAspectRatio(w, h, sceneW, sceneH)) return {0, 0, w, h};
		// shortcut: uses the game's centered aspect-fit placement, read the compositing viewport if a scene uses an offset.
		const double scale = min(double(w) / sceneW, double(h) / sceneH);
		const UINT width = min(w, max(1u, static_cast<UINT>(scale * sceneW + 0.5)));
		const UINT height = min(h, max(1u, static_cast<UINT>(scale * sceneH + 0.5)));
		return {(w - width) / 2, (h - height) / 2, width, height};
	}

	bool EnsureFg(const FrameData& fd, uint32_t keyW, uint32_t keyH)
	{
		g.fgSceneW = fd.displayW;
		g.fgSceneH = fd.displayH;
		UINT displayW = fd.displayW, displayH = fd.displayH;
		// A smaller swap chain does not need interpolation at the full upscaled scene resolution.
		if (MatchingAspectRatio(g.fgContent.width, g.fgContent.height, displayW, displayH)) {
			displayW = min(displayW, max(g.fgContent.width, keyW));
			displayH = min(displayH, max(g.fgContent.height, keyH));
		}
		uint32_t flags = (fd.createFlags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0;
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
		desc.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
		desc.header.pNext = &backend.header;
		backend.header.pNext = &version.header;
		ffxReturnCode_t rc = g.fx.CreateContext(&g.fg, &desc.header, nullptr);
		if (rc != FFX_API_RETURN_OK) {
			Log("FG ffxCreateContext failed: %u. Continuing with upscaling.", rc);
			g.fg = nullptr;
			g.fgUnavailable = true;
			CheckRemoved();
			return false;
		}
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
		Log("FG context: inputs up to %ux%u, display %ux%u, flags 0x%X: provider '%s' (id 0x%llX); GPU memory %.1f MB%s; manual dispatch, presented through the game's swap chain.",
			keyW, keyH, displayW, displayH, flags, g.stats.fgProvider, (unsigned long long)g.stats.fgProviderId, mem.totalUsageInBytes / 1048576.0, mrc == FFX_API_RETURN_OK ? "" : " (query failed)");
		return true;
	}

	void FgPrepare(const FrameData& fd, ID3D12GraphicsCommandList* l, uint32_t keyW, uint32_t keyH)
	{
		if (!fd.fg || !g.fgDll || !PresentHooked() || g.fgUnavailable)
		{
			if (g.fg && WaitIdle("FG off")) DestroyFg();
			g.fgPending = false;
			if (!g.fgOffLogged) { g.fgOffLogged = true; Log("FG off."); }
			return;
		}
		if (!EnsureFg(fd, keyW, keyH)) { g.fgPending = false; return; }
		uint64_t id = ++g.fgFrameId;
		ffxConfigureDescFrameGeneration c = {};
		c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		c.swapChain = nullptr;
		c.frameGenerationEnabled = true;
		c.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
		c.generationRect = { 0, 0, (int32_t)g.fgDisplayW, (int32_t)g.fgDisplayH };
		c.frameID = id;
		ffxReturnCode_t rc = g.fx.Configure(&g.fg, &c.header);
		if (rc != FFX_API_RETURN_OK) { if (++g.fgErrors <= 3) Log("FG configure failed: %u (frame %llu).", rc, (unsigned long long)id); g.fgPending = g.fgHavePrevious = false; return; }
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
		if (rc != FFX_API_RETURN_OK) { if (++g.fgErrors <= 3) Log("FG prepare failed: %u (frame %llu).", rc, (unsigned long long)id); g.fgPending = g.fgHavePrevious = false; return; }
		g.stats.fgPrepared++;
		g.fgPending = true;
		g.fgPendingReset = p.reset;
		if (g.fgOffLogged) { g.fgOffLogged = false; Log("FG prepared inputs for the game swap chain."); }
	}

	void Pace(LONGLONG from, double ms)
	{
		LARGE_INTEGER f, now;
		QueryPerformanceFrequency(&f);
		LONGLONG until = from + (LONGLONG)(ms * f.QuadPart / 1000.0);
		for (;;)
		{
			QueryPerformanceCounter(&now);
			LONGLONG left = until - now.QuadPart;
			if (left <= 0) return;
			if (left * 1000.0 / f.QuadPart > 1.5) Sleep(1);
			else YieldProcessor();
		}
	}

	void Upscale(const FrameData& handed)
	{
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
		if (g.d12 && dev.Get() != g.d11Base) Teardown("the game's D3D11 device changed");
		if (!g.d12 && !InitDevices((ID3D11Resource*)fd.color)) { Teardown("could not start"); Fail("the D3D12 side could not be made (above)."); return; }
		if (!EnsureShared(g.color, (ID3D11Texture2D*)fd.color, false, "color", true)
			|| !EnsureShared(g.depth, (ID3D11Texture2D*)fd.depth, false, "depth", false)
			|| !EnsureShared(g.motion, (ID3D11Texture2D*)fd.motion, false, "motion vectors", true)
			|| !EnsureShared(g.output, (ID3D11Texture2D*)fd.output, true, "output", true)) { Fail("shared textures (above)."); return; }
		if (!EnsureContext(fd)) { Fail("no FSR context (above)."); return; }
		if (fd.fg && g.fgDll) InstallPresentHook(static_cast<ID3D11Resource*>(fd.color));

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
			if (g.teardownDue) { g.teardownDue = false; Teardown("after an exception"); }
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
	void __stdcall OnRenderEvent(int eventId, void* data)
	{
		std::lock_guard<std::mutex> lock(g_renderLock);
		g.renderThread = GetCurrentThreadId();
		RunRenderEvent(eventId, data);
	}
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

ID3D12CommandQueue* BridgeQueue() { return g.queue.Get(); }

bool FgPresentInner(IDXGISwapChain* sc, UINT sync, UINT flags, PresentCall original,
	const DXGI_PRESENT_PARAMETERS* parameters, HRESULT* hr);

bool FgPresent(IDXGISwapChain* sc, UINT sync, UINT flags, PresentCall original,
	const DXGI_PRESENT_PARAMETERS* parameters, HRESULT* hr)
{
	__try
	{
		return FgPresentInner(sc, sync, flags, original, parameters, hr);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		DWORD code = GetExceptionCode();
		if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_STACK_OVERFLOW) g.fatal = true;
		g.teardownDue = true;
		g.stats.failures++;
		g.failed = true;
		Log("FSR stopped: exception 0x%08X in frame generation at present%s.", (unsigned)code, g.fatal ? " (a memory fault: off until the game restarts)" : "");
		return false;
	}
}

bool FgPresentInner(IDXGISwapChain* sc, UINT sync, UINT flags, PresentCall original,
	const DXGI_PRESENT_PARAMETERS* parameters, HRESULT* hr)
{
	std::unique_lock<std::mutex> lock(g_renderLock, std::try_to_lock);
	if (!lock || !g.fg || g.fgUnavailable || g.failed || deviceLost || (flags & DXGI_PRESENT_TEST)) return false;
	ComPtr<ID3D11Device> device;
	if (FAILED(sc->GetDevice(IID_PPV_ARGS(&device))) || device.Get() != g.d11Base) return false;
	DXGI_SWAP_CHAIN_DESC chain{};
	if (FAILED(sc->GetDesc(&chain)) || !chain.OutputWindow || !IsPresentWindow(chain.OutputWindow)) return false;
	if (!g.fgPending || sync > 4 || (flags & ~DXGI_PRESENT_ALLOW_TEARING) ||
		(parameters && (parameters->DirtyRectsCount || parameters->pScrollRect || parameters->pScrollOffset)))
	{
		g.fgPending = g.fgHavePrevious = false;
		return false;
	}
	g.fgPending = false;
	ComPtr<ID3D11Texture2D> bb;
	D3D11_TEXTURE2D_DESC bd{};
	if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) bb->GetDesc(&bd);
	if (!bd.Width || !bd.Height || !g.fgSceneW || !g.fgSceneH ||
		(bd.Width >= bd.Height) != (g.fgSceneW >= g.fgSceneH) ||
		bd.SampleDesc.Count != 1 || Typed(bd.Format) != DXGI_FORMAT_R8G8B8A8_UNORM)
	{
		static bool warned = false;
		if (!warned) {
			warned = true;
			Log("FG waiting for a compatible SDR backbuffer: camera %ux%u, swap chain %ux%u (format %u, samples %u).",
				g.fgSceneW, g.fgSceneH, bd.Width, bd.Height, bd.Format, bd.SampleDesc.Count);
		}
		g.stats.fgSkipped++;
		g.fgHavePrevious = false;
		g.fgContent = {};
		return false;
	}
	const ImageRect content = FitScene(bd.Width, bd.Height, g.fgSceneW, g.fgSceneH);
	const ImageRect previous = g.fgContent;
	const bool letterboxed = content.width != bd.Width || content.height != bd.Height;
	const bool scaled = letterboxed || bd.Width != g.fgDisplayW || bd.Height != g.fgDisplayH;
	g.fgContent = content;
	if (previous.x != content.x || previous.y != content.y ||
		previous.width != content.width || previous.height != content.height)
	{
		if (letterboxed)
			Log("FG centered viewport: %u,%u %ux%u in %ux%u.", content.x, content.y, content.width, content.height, bd.Width, bd.Height);
		if (previous.width) {
			g.fgHavePrevious = false;
			g.stats.fgSkipped++;
			return false;
		}
	}
	if (GetCurrentThreadId() != g.renderThread && !g.fgCrossThread)
	{
		ComPtr<ID3D11Multithread> multithread;
		if ((device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) || FAILED(g.ctx.As(&multithread))) {
			g.fgUnavailable = true;
			Log("FG disabled: the render and present threads cannot share the D3D11 context.");
			return false;
		}
		multithread->SetMultithreadProtected(TRUE);
		g.fgCrossThread = true;
	}
	LARGE_INTEGER entry, qf;
	QueryPerformanceCounter(&entry);
	QueryPerformanceFrequency(&qf);
	if (g.fgLastEntry)
	{
		double dt = (double)(entry.QuadPart - g.fgLastEntry);
		if (dt > 0 && dt < qf.QuadPart) g.fgIntervalTicks = g.fgIntervalTicks == 0 ? dt : g.fgIntervalTicks + (dt - g.fgIntervalTicks) / 8.0;
	}
	g.fgLastEntry = entry.QuadPart;
	g.stats.fgIntervalMs = (float)(g.fgIntervalTicks * 1000.0 / qf.QuadPart);
	if (g.fgRefreshFrames++ % 600 == 0)
	{
		MONITORINFOEXW mi = {};
		mi.cbSize = sizeof mi;
		DEVMODEW dm = {};
		dm.dmSize = sizeof dm;
		if (GetMonitorInfoW(MonitorFromWindow(chain.OutputWindow, MONITOR_DEFAULTTONEAREST), &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
			g.stats.fgRefreshHz = (float)dm.dmDisplayFrequency;
	}
	double refreshMs = g.stats.fgRefreshHz > 1 ? 1000.0 / g.stats.fgRefreshHz : 0.0;
	double intervalMs = g.fgIntervalTicks * 1000.0 / qf.QuadPart;
	if (sync < 2 && refreshMs > 0 && intervalMs > 0 && intervalMs < 2.0 * refreshMs * 0.98)
	{
		g.stats.fgNotNeeded++;
		g.fgHavePrevious = false;
		return false;
	}

	bool showGenerated = !g.fgPendingReset && g.fgHavePrevious;
	bool ok = EnsureShared(g.present, bb.Get(), false, "present colour", false) && EnsureShared(g.gen, bb.Get(), true, "generated frame", false);
	if (ok && scaled)
		ok = EnsureScale() &&
			MakeShared(g.fgColor, g.fgDisplayW, g.fgDisplayH, DXGI_FORMAT_R8G8B8A8_UNORM, true, "scaled FG input", false, -1) &&
			MakeShared(g.fgOutput, g.fgDisplayW, g.fgDisplayH, DXGI_FORMAT_R8G8B8A8_UNORM, true, "scaled FG output", false, -1);
	if (!ok) { g.fgUnavailable = true; Log("FG disabled: could not allocate backbuffer resources."); return false; }
	const Shared& source = scaled ? g.fgColor : g.present;
	const Shared& output = scaled ? g.fgOutput : g.gen;

	int slot = (int)(g.submits % Slots);
	if (!WaitOut(g.allocFence[slot], FrameWaitMs, "FG command allocator reuse")) { Fail("the GPU stopped making progress (above)."); return false; }
	ReadTimestamps(slot);
	ID3D12GraphicsCommandList* l = g.list[slot].Get();
	if (!Check(g.alloc[slot]->Reset(), "FG command allocator Reset") || !Check(l->Reset(g.alloc[slot].Get(), nullptr), "FG command list Reset")) { Fail("FG command list (above)."); return false; }

	g.ctx->CopyResource(g.present.t11.Get(), bb.Get());
	if (letterboxed && showGenerated)
		g.ctx->CopyResource(g.gen.t11.Get(), bb.Get());
	uint64_t ready = ++g.inValue;
	if (!Check(g.ctx->Signal(g.in11.Get(), ready), "D3D11 Signal (FG)")) { l->Close(); Fail("fence (above)."); return false; }
	g.ctx->Flush();

	const ImageRect interpolation{0, 0, g.fgDisplayW, g.fgDisplayH};
	if (scaled) ScaleFrame(l, g.present, g.fgColor, content, interpolation, slot * 4);
	D3D12_RESOURCE_BARRIER barriers[2];
	Barrier(source.t12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, barriers[0]);
	Barrier(output.t12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, barriers[1]);
	l->ResourceBarrier(_countof(barriers), barriers);
	ffxDispatchDescFrameGeneration d = {};
	d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
	d.commandList = l;
	d.presentColor = ffxApiGetResourceDX12(source.t12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
	d.outputs[0] = ffxApiGetResourceDX12(output.t12.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
	d.numGeneratedFrames = 1;
	d.reset = g.fgPendingReset || !g.fgHavePrevious;
	d.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
	d.minMaxLuminance[0] = 0.0f;
	d.minMaxLuminance[1] = 1.0f;
	d.generationRect = { 0, 0, (int32_t)g.fgDisplayW, (int32_t)g.fgDisplayH };
	d.frameID = g.fgFrameId;
	l->EndQuery(g.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
	ffxReturnCode_t rc = g.fx.Dispatch(&g.fg, &d.header);
	l->EndQuery(g.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
	g.stats.fgLastResult = (int32_t)rc;
	for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
	l->ResourceBarrier(_countof(barriers), barriers);
	if (scaled && rc == FFX_API_RETURN_OK && showGenerated && d.numGeneratedFrames)
		ScaleFrame(l, g.fgOutput, g.gen, interpolation, content, slot * 4 + 2);
	l->ResolveQueryData(g.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, g.readback.Get(), slot * 2 * sizeof(uint64_t));
	if (!Check(l->Close(), "FG command list Close")) { Fail("FG command list (above)."); return false; }
	if (!Check(g.queue->Wait(g.in12.Get(), ready), "queue Wait (FG)")) { Fail("fence (above)."); return false; }
	ID3D12CommandList* lists[] = { l };
	g.queue->ExecuteCommandLists(1, lists);
	uint64_t done = ++g.outValue;
	if (!Check(g.queue->Signal(g.out12.Get(), done), "queue Signal (FG)")) { Fail("fence (above)."); return false; }
	g.allocFence[slot] = done;
	g.slotTimed[slot] = true;
	g.slotFg[slot] = true;
	g.submits++;
	if (!Check(g.ctx->Wait(g.out11.Get(), done), "D3D11 Wait (FG)")) { Fail("fence (above)."); return false; }
	if (rc != FFX_API_RETURN_OK)
	{
		if (++g.fgErrors <= 3) Log("FG dispatch failed: %u (frame %llu).", rc, (unsigned long long)g.fgFrameId);
		g.stats.fgSkipped++;
		g.fgHavePrevious = false;
		CheckRemoved();
		return false;
	}
	g.fgHavePrevious = true;
	if (!showGenerated || !d.numGeneratedFrames) { g.stats.fgSkipped++; return false; }

	g.ctx->CopyResource(bb.Get(), g.gen.t11.Get());
	// A flip-model Present at interval zero can discard the queued interpolation before scanout.
	const bool syncPair = sync == 0 && (chain.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
		chain.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
	const UINT pairFlags = syncPair ? flags & ~DXGI_PRESENT_ALLOW_TEARING : flags;
	const UINT generatedSync = syncPair ? 1 : sync < 2 ? sync : sync / 2;
	const UINT realSync = sync < 2 ? generatedSync : sync - generatedSync;
	const HRESULT generated = original(sc, generatedSync, pairFlags, parameters);
	if (generated != S_OK)
	{
		g.ctx->CopyResource(bb.Get(), g.present.t11.Get());
		g.fgHavePrevious = false;
		*hr = generated;
		return true;
	}
	g.stats.fgGenerated++;
	static bool reportedSync = false;
	if (syncPair && !reportedSync) {
		reportedSync = true;
		Log("FG synchronizes flip-model frame pairs to vblank.");
	}
	LARGE_INTEGER t1;
	QueryPerformanceCounter(&t1);
	double half = intervalMs * 0.5;
	if (sync < 2 && half > 0 && (sync == 0 || refreshMs == 0 || half > refreshMs)) Pace(entry.QuadPart, half);
	LARGE_INTEGER t2;
	QueryPerformanceCounter(&t2);
	Average(g.stats.fgPaceMs, (t2.QuadPart - t1.QuadPart) * 1000.0 / qf.QuadPart);
	ComPtr<ID3D11Texture2D> bb2;
	if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&bb2))) && bb2)
	{
		g.ctx->CopyResource(bb2.Get(), g.present.t11.Get());
		*hr = original(sc, realSync, pairFlags, parameters);
		g.stats.fgReal++;
	}
	else *hr = DXGI_ERROR_INVALID_CALL;
	LARGE_INTEGER t3;
	QueryPerformanceCounter(&t3);
	Average(g.stats.fgHookMs, (t3.QuadPart - entry.QuadPart) * 1000.0 / qf.QuadPart);
	Publish();
	return true;
}

EXPORT int FSR4_Load(const wchar_t* dir)
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

EXPORT int FSR4_LoadFrameGeneration(const wchar_t* dir)
{
	if (g.fgDll) return 1;
	std::wstring d(dir);
	if (!d.empty() && d.back() != L'\\') d += L'\\';
	g.fgDll = LoadLibraryExW((d + L"amd_fidelityfx_framegeneration_dx12.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!g.fgDll) {
		Log("Could not load amd_fidelityfx_framegeneration_dx12.dll: error %lu. Continuing with upscaling.", GetLastError());
		return 0;
	}
	timeBeginPeriod(1);
	return 1;
}

EXPORT void* FSR4_GetRenderEventFunc() { return (void*)&OnRenderEvent; }

EXPORT int FSR4_PollLog(char* buf, int cap)
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

EXPORT void FSR4_GetStats(Stats* out)
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

EXPORT int FSR4_LayoutField(int which, int i, const char** name, uint32_t* offset)
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

EXPORT int FSR4_Failed() { return g.fatal ? 2 : g.failed ? 1 : 0; }

EXPORT int FSR4_ExposureWanted() { return g.exposureRequired ? 1 : 0; }

EXPORT uint64_t FSR4_GetViews(void** color, void** motion, void** output)
{
	std::lock_guard<std::mutex> l(g_viewLock);
	*color = g.color.srv.Get(); *motion = g.motion.srv.Get(); *output = g.output.srv.Get();
	return *color && *motion && *output ? g_viewGen : 0;
}

EXPORT uint64_t FSR4_GetHudlessView(void** hudless)
{
	std::lock_guard<std::mutex> l(g_viewLock);
	*hudless = nullptr;
	return 0;
}

EXPORT void FSR4_ViewsInUse(uint64_t gen)
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

EXPORT void FSR4_ClearFailure() { if (!g.fatal) g.failed = false; }
