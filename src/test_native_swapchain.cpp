// Standalone proof of concept: a D3D11-rendered texture is shared with D3D12,
// copied to an AMD FSR FG SwapChain and presented in a separate test window.
// It does not replace or intercept Unity's D3D11 swap chain.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "dx12/ffx_api_framegeneration_dx12.h"

using Microsoft::WRL::ComPtr;

static void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) {
        printf("%s failed: HRESULT 0x%08X\n", operation, static_cast<unsigned>(hr));
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

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) {
        puts("Usage: test_native_swapchain.exe <official AMD FSR runtime DLL directory>");
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
    if (!fx.CreateContext || !fx.DestroyContext) return 4;

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
        ComPtr<ID3D11Device5> d11v5;
        ComPtr<ID3D11DeviceContext4> ctx;
        Check(d11.As(&d11v5), "ID3D11Device5");
        Check(immediate.As(&ctx), "ID3D11DeviceContext4");

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        ComPtr<ID3D12CommandQueue> queue;
        Check(d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"GakuFSR4NativeSwapchainTest";
        RegisterClassW(&wc);
        HWND window = CreateWindowExW(0, wc.lpszClassName, L"AMD FG SwapChain interop test",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 400, 360,
            nullptr, nullptr, wc.hInstance, nullptr);
        if (!window) throw std::runtime_error("Test window creation failed");
        ShowWindow(window, SW_SHOWNORMAL);

        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = swapDesc.Height = 256;
        swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = 2;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
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
        puts("AMD native D3D12 FG SwapChain created. Creating shared D3D11 content...");

        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = textureDesc.Height = 256;
        textureDesc.MipLevels = textureDesc.ArraySize = 1;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        ComPtr<ID3D11Texture2D> input11;
        Check(d11->CreateTexture2D(&textureDesc, nullptr, &input11), "D3D11 CreateTexture2D");
        ComPtr<ID3D11RenderTargetView> target;
        Check(d11->CreateRenderTargetView(input11.Get(), nullptr, &target), "CreateRenderTargetView");
        const float green[]{0.1f, 0.8f, 0.3f, 1.0f};
        ctx->ClearRenderTargetView(target.Get(), green);
        ComPtr<IDXGIResource1> dxgiResource;
        Check(input11.As(&dxgiResource), "IDXGIResource1");
        HANDLE textureHandle = nullptr;
        Check(dxgiResource->CreateSharedHandle(nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &textureHandle), "CreateSharedHandle");
        ComPtr<ID3D12Resource> input12;
        const HRESULT openResult = d12->OpenSharedHandle(textureHandle, IID_PPV_ARGS(&input12));
        CloseHandle(textureHandle);
        Check(openResult, "D3D12 OpenSharedHandle");

        ComPtr<ID3D11Fence> fence11;
        Check(d11v5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11)), "D3D11 CreateFence");
        HANDLE fenceHandle = nullptr;
        Check(fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle), "Fence CreateSharedHandle");
        ComPtr<ID3D12Fence> fence12;
        const HRESULT fenceOpenResult = d12->OpenSharedHandle(fenceHandle, IID_PPV_ARGS(&fence12));
        CloseHandle(fenceHandle);
        Check(fenceOpenResult, "D3D12 OpenSharedFence");
        Check(ctx->Signal(fence11.Get(), 1), "D3D11 Signal");
        ctx->Flush();
        Check(queue->Wait(fence12.Get(), 1), "D3D12 queue Wait");

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        Check(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        Check(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list)), "CreateCommandList");
        ComPtr<ID3D12Resource> backbuffer;
        Check(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)), "GetBuffer");
        auto b1 = Transition(input12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto b2 = Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_RESOURCE_BARRIER barriers[]{b1,b2};
        list->ResourceBarrier(2, barriers);
        list->CopyResource(backbuffer.Get(), input12.Get());
        b1 = Transition(input12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        b2 = Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        barriers[0] = b1;barriers[1] = b2;
        list->ResourceBarrier(2, barriers);
        Check(list->Close(), "CommandList Close");
        ID3D12CommandList* execute[]{list.Get()};
        queue->ExecuteCommandLists(1, execute);
        Check(swapchain->Present(1, 0), "AMD FG SwapChain Present");
        Check(queue->Signal(fence12.Get(), 2), "D3D12 Signal");
        for (int n = 0; n < 500 && fence12->GetCompletedValue() < 2; ++n) Sleep(10);
        if (fence12->GetCompletedValue() < 2) throw std::runtime_error("GPU fence did not finish");
        puts("PASS: D3D11 colour -> shared texture -> D3D12 copy -> AMD native SwapChain Present.");
        puts("This does not test generated frames, driver-assisted pacing, or Unity SwapChain interception.");
        fx.DestroyContext(&swapchainContext, nullptr);
        swapchain.Reset();
        DestroyWindow(window);
        return 0;
    } catch (const std::exception& e) {
        printf("Native FG SwapChain proof of concept failed: %s\n", e.what());
        return 6;
    }
}
