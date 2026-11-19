// Standalone proof of concept: render through D3D11 into an AMD FSR FG SwapChain,
// using either a shared texture or D3D11On12 wrapped backbuffers.
// It does not replace or intercept Unity's D3D11 swap chain.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_framegeneration.h"
#include "dx12/ffx_api_framegeneration_dx12.h"

using Microsoft::WRL::ComPtr;

static void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) {
        printf("%s failed: HRESULT 0x%08X\n", operation, static_cast<unsigned>(hr));
        throw std::runtime_error(operation);
    }
}

static void CheckFx(ffxReturnCode_t rc, const char* operation)
{
    if (rc != FFX_API_RETURN_OK) {
        printf("%s failed: FFX error %u\n", operation, static_cast<unsigned>(rc));
        throw std::runtime_error(operation);
    }
}

static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                          D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

static constexpr UINT frameCount = 480;
static constexpr UINT bufferCount = 2;

// The test uses flat depth and static vectors to validate the dispatch and presentation path.
static ComPtr<ID3D12Resource> MakeInput(ID3D12Device* device, ID3D12GraphicsCommandList* list,
    UINT width, UINT height, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const float (&clear)[4])
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> input;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&input)), "Create FG test input");
    device->CreateRenderTargetView(input.Get(), nullptr, rtv);
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    const auto barrier = Transition(input.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &barrier);
    return input;
}

struct Interpolation {
    const ffxFunctions& fx;
    ffxContext swapchainContext;
    ffxContext context = nullptr;
    ffxConfigureDescFrameGeneration config{};
    std::atomic<unsigned> generated{0};
    std::atomic<unsigned> failure{FFX_API_RETURN_OK};

    void Configure(uint64_t frameID)
    {
        config.frameID = frameID;
        CheckFx(fx.Configure(&context, &config.header), "Configure native FG");
    }

    static ffxReturnCode_t Dispatch(ffxDispatchDescFrameGeneration* params, void* user)
    {
        auto& self = *static_cast<Interpolation*>(user);
        const auto result = self.fx.Dispatch(&self.context, &params->header);
        if (result == FFX_API_RETURN_OK) self.generated += params->numGeneratedFrames;
        else self.failure = result;
        return result;
    }

    ffxReturnCode_t Drain()
    {
        if (!context) return FFX_API_RETURN_OK;
        ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
        wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
        const auto waitResult = fx.Dispatch(&swapchainContext, &wait.header);
        config.frameGenerationEnabled = false;
        const auto disableResult = fx.Configure(&context, &config.header);
        // The runtime may leave the caller's handle unchanged after successful destruction.
        auto closing = std::exchange(context, nullptr);
        const auto destroyResult = fx.DestroyContext(&closing, nullptr);
        if (waitResult != FFX_API_RETURN_OK) return waitResult;
        if (disableResult != FFX_API_RETURN_OK) return disableResult;
        return destroyResult;
    }

    ~Interpolation() { Drain(); }
};

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool enableFg = false, borderless = false, useOn12 = false;
    for (int i = 2; i < argc; ++i) {
        const std::wstring option = argv[i];
        if (option == L"--fg" && !enableFg) enableFg = true;
        else if (option == L"--borderless" && !borderless) borderless = true;
        else if (option == L"--on12" && !useOn12) useOn12 = true;
        else return 2;
    }
    if (argc < 2) {
        puts("Usage: test_native_swapchain.exe <official AMD FSR runtime DLL directory> [--fg] [--borderless] [--on12]");
        return 2;
    }
    std::wstring directory = argv[1];
    if (!directory.empty() && directory.back() != L'\\') directory += L'\\';
    HMODULE loader = LoadLibraryExW((directory + L"amd_fidelityfx_loader_dx12.dll").c_str(),
                                    nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE fgLibrary = LoadLibraryExW((directory + L"amd_fidelityfx_framegeneration_dx12.dll").c_str(),
                                       nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!loader || !fgLibrary) {
        printf("AMD FSR loader/framegeneration DLL loading failed: Win32 %lu\n", GetLastError());
        return 3;
    }
    ffxFunctions fx{};
    ffxLoadFunctions(&fx, loader);
    if (!fx.CreateContext || !fx.DestroyContext || !fx.Configure || !fx.Dispatch) return 4;

    try {
        ComPtr<IDXGIFactory4> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        ComPtr<ID3D12Device> d12;
        ComPtr<ID3D11Device> d11;
        ComPtr<ID3D11DeviceContext> immediate;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT n = 0; factory->EnumAdapters1(n, &adapter) != DXGI_ERROR_NOT_FOUND; ++n) {
            DXGI_ADAPTER_DESC1 info{};
            if (FAILED(adapter->GetDesc1(&info)) || (info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
                FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d12))) ||
                FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                    D3D11_SDK_VERSION, &d11, nullptr, &immediate))) {
                d12.Reset();d11.Reset();adapter.Reset();
                continue;
            }
            printf("Using adapter: %ls\n", info.Description);
            break;
        }
        if (!d11 || !d12) throw std::runtime_error("No D3D11/D3D12 compatible hardware adapter");
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        ComPtr<ID3D12CommandQueue> queue;
        Check(d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        ComPtr<ID3D11On12Device> on12;
        if (useOn12) {
            IUnknown* queues[]{queue.Get()};
            Check(D3D11On12CreateDevice(d12.Get(), 0, nullptr, 0, queues, 1, 0,
                d11.ReleaseAndGetAddressOf(), immediate.ReleaseAndGetAddressOf(), nullptr), "D3D11On12CreateDevice");
            Check(d11.As(&on12), "ID3D11On12Device");
        }
        ComPtr<ID3D11DeviceContext4> ctx;
        Check(immediate.As(&ctx), "ID3D11DeviceContext4");

        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"GakuFSR4NativeSwapchainTest";
        RegisterClassW(&wc);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        UINT width = 960, height = 540;
        DWORD style = WS_OVERLAPPEDWINDOW;
        RECT bounds{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        UINT presentFlags = 0, swapFlags = 0;
        if (borderless) {
            POINT cursor{};
            GetCursorPos(&cursor);
            MONITORINFO monitor{sizeof monitor};
            if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor))
                throw std::runtime_error("Could not resolve the monitor for the borderless test");
            bounds = monitor.rcMonitor;
            width = bounds.right - bounds.left;
            height = bounds.bottom - bounds.top;
            style = WS_POPUP;
            ComPtr<IDXGIFactory5> factory5;
            BOOL tearing = FALSE;
            Check(factory.As(&factory5), "IDXGIFactory5");
            Check(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing),
                "Check tearing support");
            if (!tearing) throw std::runtime_error("Borderless test requires DXGI tearing support");
            swapFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            presentFlags = DXGI_PRESENT_ALLOW_TEARING;
        } else {
            AdjustWindowRect(&bounds, style, FALSE);
        }
        HWND window = CreateWindowExW(0, wc.lpszClassName, L"AMD FG SwapChain interop test",
            style, borderless ? bounds.left : CW_USEDEFAULT, borderless ? bounds.top : CW_USEDEFAULT,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, wc.hInstance, nullptr);
        if (!window) throw std::runtime_error("Test window creation failed");
        std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)> ownedWindow(window, DestroyWindow);
        ShowWindow(window, SW_SHOWNORMAL);
        if (borderless) {
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(window);
        }

        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = width;
        swapDesc.Height = height;
        swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = bufferCount;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.Flags = swapFlags;
        ffxCreateContextDescFrameGenerationSwapChainVersionDX12 version{};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
        version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
        IDXGISwapChain4* rawSwapchain = nullptr;
        ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
        create.header.pNext = &version.header;
        create.swapchain = &rawSwapchain;
        create.hwnd = window;
        create.desc = &swapDesc;
        create.dxgiFactory = factory.Get();
        create.gameQueue = queue.Get();
        ffxContext swapchainContext = nullptr;
        const auto result = fx.CreateContext(&swapchainContext, &create.header, nullptr);
        if (result != FFX_API_RETURN_OK || !rawSwapchain) {
            printf("AMD native FG SwapChain creation failed: %u\n", static_cast<unsigned>(result));
            return 5;
        }
        ComPtr<IDXGISwapChain4> swapchain;
        swapchain.Attach(rawSwapchain);
        const auto destroySwapchain = [&fx](ffxContext context) { fx.DestroyContext(&context, nullptr); };
        std::unique_ptr<void, decltype(destroySwapchain)> ownedSwapchain(swapchainContext, destroySwapchain);
        printf("AMD native D3D12 FG SwapChain: %ux%u, %s, FG %s, SyncInterval 0, tearing %s.\n",
            width, height, borderless ? "borderless" : "windowed", enableFg ? "on" : "off",
            presentFlags ? "allowed" : "disabled");
        puts(useOn12 ? "Colour path: D3D11On12 wrapped backbuffers." : "Colour path: shared native D3D11 texture.");

        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D12Resource> input12;
        ComPtr<ID3D11Fence> fence11;
        ComPtr<ID3D12Fence> fence12;
        struct WrappedBuffer {
            ComPtr<ID3D11Resource> resource;
            ComPtr<ID3D11RenderTargetView> target;
        };
        std::array<WrappedBuffer, bufferCount> wrapped;
        if (useOn12) {
            const D3D11_RESOURCE_FLAGS flags{D3D11_BIND_RENDER_TARGET};
            for (UINT i = 0; i < swapDesc.BufferCount; ++i) {
                ComPtr<ID3D12Resource> buffer;
                Check(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffer)), "Get AMD backbuffer for D3D11On12");
                Check(on12->CreateWrappedResource(buffer.Get(), &flags, D3D12_RESOURCE_STATE_PRESENT,
                    D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&wrapped[i].resource)), "Wrap AMD backbuffer");
                Check(d11->CreateRenderTargetView(wrapped[i].resource.Get(), nullptr, &wrapped[i].target),
                    "Create wrapped backbuffer RTV");
            }
            Check(d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence12)), "Create D3D12 frame fence");
        } else {
            D3D11_TEXTURE2D_DESC textureDesc{};
            textureDesc.Width = width;
            textureDesc.Height = height;
            textureDesc.MipLevels = textureDesc.ArraySize = 1;
            textureDesc.SampleDesc.Count = 1;
            textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            textureDesc.Usage = D3D11_USAGE_DEFAULT;
            textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            ComPtr<ID3D11Texture2D> input11;
            Check(d11->CreateTexture2D(&textureDesc, nullptr, &input11), "D3D11 CreateTexture2D");
            Check(d11->CreateRenderTargetView(input11.Get(), nullptr, &target), "CreateRenderTargetView");
            ComPtr<IDXGIResource1> dxgiResource;
            Check(input11.As(&dxgiResource), "IDXGIResource1");
            HANDLE textureHandle = nullptr;
            Check(dxgiResource->CreateSharedHandle(nullptr,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &textureHandle), "CreateSharedHandle");
            const HRESULT openResult = d12->OpenSharedHandle(textureHandle, IID_PPV_ARGS(&input12));
            CloseHandle(textureHandle);
            Check(openResult, "D3D12 OpenSharedHandle");

            ComPtr<ID3D11Device5> d11v5;
            Check(d11.As(&d11v5), "ID3D11Device5");
            Check(d11v5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11)), "D3D11 CreateFence");
            HANDLE fenceHandle = nullptr;
            Check(fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle), "Fence CreateSharedHandle");
            const HRESULT fenceOpenResult = d12->OpenSharedHandle(fenceHandle, IID_PPV_ARGS(&fence12));
            CloseHandle(fenceHandle);
            Check(fenceOpenResult, "D3D12 OpenSharedFence");
        }
        std::unique_ptr<void, decltype(&CloseHandle)> fenceEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle);
        if (!fenceEvent) throw std::runtime_error("Could not create GPU fence event");
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        Check(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        Check(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list)), "CreateCommandList");
        ComPtr<ID3D12DescriptorHeap> rtvs;
        ComPtr<ID3D12Resource> depth, motion;
        Interpolation fg{fx, swapchainContext};
        if (enableFg) {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = 2;
            Check(d12->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvs)), "Create RTV heap");
            auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
            const float flatDepth[]{0.5f, 0.0f, 0.0f, 0.0f};
            const float zeroMotion[]{0.0f, 0.0f, 0.0f, 0.0f};
            depth = MakeInput(d12.Get(), list.Get(), width, height, DXGI_FORMAT_R32_FLOAT, rtv, flatDepth);
            rtv.ptr += d12->GetDescriptorHandleIncrementSize(heapDesc.Type);
            motion = MakeInput(d12.Get(), list.Get(), width, height, DXGI_FORMAT_R16G16_FLOAT, rtv, zeroMotion);

            ffxCreateContextDescFrameGenerationVersion fgVersion{};
            fgVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
            fgVersion.version = FFX_FRAMEGENERATION_VERSION;
            ffxCreateBackendDX12Desc backend{};
            backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
            backend.header.pNext = &fgVersion.header;
            backend.device = d12.Get();
            ffxCreateContextDescFrameGeneration desc{};
            desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
            desc.header.pNext = &backend.header;
            desc.displaySize = desc.maxRenderSize = {width, height};
            desc.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
            CheckFx(fx.CreateContext(&fg.context, &desc.header, nullptr), "Create native FG");
            fg.config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            fg.config.swapChain = swapchain.Get();
            fg.config.frameGenerationCallback = Interpolation::Dispatch;
            fg.config.frameGenerationCallbackUserContext = &fg;
            fg.config.frameGenerationEnabled = true;
            fg.config.generationRect = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
        }

        const auto start = std::chrono::steady_clock::now();
        for (UINT frame = 0; frame < frameCount; ++frame) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            if (!IsWindow(window)) throw std::runtime_error("Test window closed");
            std::this_thread::sleep_until(start + std::chrono::microseconds(frame * 1000000ull / 60));
            if (frame) {
                Check(allocator->Reset(), "Reset command allocator");
                Check(list->Reset(allocator.Get(), nullptr), "Reset command list");
            }
            const float background[]{0.1f, 0.2f, 0.3f, 1.0f};
            const float green[]{0.1f, 0.8f, 0.3f, 1.0f};
            const auto boxWidth = static_cast<LONG>(width / 10);
            const auto left = static_cast<LONG>(frame * (width / 160) % (width - boxWidth));
            const D3D11_RECT rect{left, static_cast<LONG>(height * 2 / 5), left + boxWidth,
                static_cast<LONG>(height * 3 / 5)};
            const UINT index = swapchain->GetCurrentBackBufferIndex();
            const auto draw = [&](ID3D11RenderTargetView* view) {
                ctx->ClearRenderTargetView(view, background);
                ctx->ClearView(view, green, &rect, 1);
            };
            const uint64_t ready = 2ull * frame + 1;
            if (useOn12) {
                auto* buffer = wrapped.at(index).resource.Get();
                on12->AcquireWrappedResources(&buffer, 1);
                draw(wrapped[index].target.Get());
                on12->ReleaseWrappedResources(&buffer, 1);
                ctx->Flush();
            } else {
                draw(target.Get());
                Check(ctx->Signal(fence11.Get(), ready), "D3D11 Signal");
                ctx->Flush();
                Check(queue->Wait(fence12.Get(), ready), "D3D12 queue Wait");

                ComPtr<ID3D12Resource> backbuffer;
                Check(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)), "GetBuffer");
                const D3D12_RESOURCE_BARRIER before[]{
                    Transition(input12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)};
                list->ResourceBarrier(2, before);
                list->CopyResource(backbuffer.Get(), input12.Get());
                const D3D12_RESOURCE_BARRIER after[]{
                    Transition(input12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
                    Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT)};
                list->ResourceBarrier(2, after);
            }
            if (enableFg) {
                fg.Configure(frame + 1);
                ffxDispatchDescFrameGenerationPrepareV2 prepare{};
                prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
                prepare.frameID = fg.config.frameID;
                prepare.commandList = list.Get();
                prepare.renderSize = {width, height};
                prepare.motionVectorScale = {static_cast<float>(width), static_cast<float>(height)};
                prepare.frameTimeDelta = 1000.0f / 60;
                prepare.reset = frame == 0;
                prepare.cameraNear = 0.1f;
                prepare.cameraFar = 100.0f;
                prepare.cameraFovAngleVertical = 1.0f;
                prepare.viewSpaceToMetersFactor = 1.0f;
                prepare.cameraUp[1] = prepare.cameraRight[0] = prepare.cameraForward[2] = 1.0f;
                prepare.depth = ffxApiGetResourceDX12(depth.Get());
                prepare.motionVectors = ffxApiGetResourceDX12(motion.Get());
                CheckFx(fx.Dispatch(&fg.context, &prepare.header), "Prepare native FG");
            }
            Check(list->Close(), "CommandList Close");
            ID3D12CommandList* execute[]{list.Get()};
            queue->ExecuteCommandLists(1, execute);
            const HRESULT present = swapchain->Present(0, presentFlags);
            Check(queue->Signal(fence12.Get(), ready + 1), "D3D12 Signal");
            Check(fence12->SetEventOnCompletion(ready + 1, fenceEvent.get()), "SetEventOnCompletion");
            if (WaitForSingleObject(fenceEvent.get(), 5000) != WAIT_OBJECT_0)
                throw std::runtime_error("GPU fence did not finish");
            Check(present, "AMD FG SwapChain Present");
        }
        CheckFx(fg.Drain(), "Drain native FG");
        if (useOn12) {
            for (auto& buffer : wrapped) {
                buffer.target.Reset();
                buffer.resource.Reset();
            }
            ctx->Flush();
        }
        CheckFx(fg.failure.load(), "Native FG callback");
        if (enableFg && fg.generated == 0) throw std::runtime_error("No native FG output was dispatched");
        printf("PASS: %u D3D11 frames presented through the AMD native SwapChain, %u interpolated frames dispatched.\n",
            frameCount, fg.generated.load());
        return 0;
    } catch (const std::exception& e) {
        printf("Native FG SwapChain proof of concept failed: %s\n", e.what());
        return 6;
    }
}
