#include "NativeSwapchain.h"
#include <wrl/client.h>
#include <atomic>
#include <new>
#include <utility>

namespace {
using Microsoft::WRL::ComPtr;

class NativeSwapchain final : public IDXGISwapChain4 {
    std::atomic<ULONG> references{1};
    NativeSwapchainState state;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Texture2D> texture;
    DXGI_SWAP_CHAIN_DESC1 requested;

    HRESULT MakeBuffer(UINT width, UINT height, ComPtr<ID3D11Texture2D>& buffer) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        // Unity can request an sRGB view of the UNORM swap-chain buffer.
        // D3D11 grants that exception to DXGI buffers, so this ordinary texture must be typeless.
        desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if(requested.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        return device->CreateTexture2D(&desc, nullptr, &buffer);
    }

public:
    NativeSwapchain(ID3D11Device* device, const DXGI_SWAP_CHAIN_DESC1& desc)
        : device(device), requested(desc) {}
    ~NativeSwapchain() { DestroyNativeSwapchain(state); }

    HRESULT Initialize(IDXGIFactory2* factory, HWND window, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen) {
        if(!requested.Width || !requested.Height) {
            RECT client{};
            if(!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
                return DXGI_ERROR_UNSUPPORTED;
            if(!requested.Width) requested.Width = static_cast<UINT>(client.right);
            if(!requested.Height) requested.Height = static_cast<UINT>(client.bottom);
        }
        const HRESULT hr = MakeBuffer(requested.Width, requested.Height, texture);
        return FAILED(hr) ? hr : CreateNativeSwapchain(factory, device.Get(), window, requested, fullscreen, state);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if(!value) return E_POINTER;
        *value = nullptr;
        if(iid != __uuidof(IUnknown) && iid != __uuidof(IDXGIObject) && iid != __uuidof(IDXGIDeviceSubObject)
            && iid != __uuidof(IDXGISwapChain) && iid != __uuidof(IDXGISwapChain1)
            && iid != __uuidof(IDXGISwapChain2) && iid != __uuidof(IDXGISwapChain3)
            && iid != __uuidof(IDXGISwapChain4)) return E_NOINTERFACE;
        *value = static_cast<IDXGISwapChain4*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --references;
        if(!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID id, UINT size, const void* data) override {
        return state.swapchain->SetPrivateData(id, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID id, const IUnknown* data) override {
        return state.swapchain->SetPrivateDataInterface(id, data);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID id, UINT* size, void* data) override {
        return state.swapchain->GetPrivateData(id, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID id, void** parent) override {
        return state.swapchain->GetParent(id, parent);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID id, void** result) override {
        return device->QueryInterface(id, result);
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID id, void** result) override {
        if(!result) return E_POINTER;
        *result = nullptr;
        // D3D11 renders to buffer 0; the backend rotates the D3D12 presentation buffers.
        return index == 0 ? texture->QueryInterface(id, result) : DXGI_ERROR_INVALID_CALL;
    }
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override {
        return PresentNativeSwapchain(state, texture.Get(), sync, flags, nullptr);
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) override {
        return PresentNativeSwapchain(state, texture.Get(), sync, flags, params);
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override {
        if(format != DXGI_FORMAT_UNKNOWN && format != requested.Format) return DXGI_ERROR_UNSUPPORTED;
        if(!width || !height) {
            HWND window{};
            RECT client{};
            if(FAILED(GetHwnd(&window)) || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
                return DXGI_ERROR_INVALID_CALL;
            if(!width) width = static_cast<UINT>(client.right);
            if(!height) height = static_cast<UINT>(client.bottom);
        }
        ComPtr<ID3D11Texture2D> replacement;
        HRESULT hr = MakeBuffer(width, height, replacement);
        if(SUCCEEDED(hr)) hr = ResizeNativeSwapchain(state, count, width, height, format, flags);
        if(SUCCEEDED(hr)) {
            texture = std::move(replacement);
            requested.Width = width;
            requested.Height = height;
            if(count) requested.BufferCount = count;
            requested.Flags = flags;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
        const UINT* masks, IUnknown* const* queues) override {
        if(masks || queues) return DXGI_ERROR_UNSUPPORTED;
        return ResizeBuffers(count, width, height, format, flags);
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override {
        return state.swapchain->GetDesc(desc);
    }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override { return state.swapchain->GetDesc1(desc); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override {
        return state.swapchain->GetFullscreenDesc(desc);
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* window) override { return state.swapchain->GetHwnd(window); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID id, void** result) override {
        return state.swapchain->GetCoreWindow(id, result);
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* output) override {
        return state.swapchain->SetFullscreenState(fullscreen, output);
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** output) override {
        return state.swapchain->GetFullscreenState(fullscreen, output);
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* target) override { return state.swapchain->ResizeTarget(target); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override { return state.swapchain->GetContainingOutput(output); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override {
        return state.swapchain->GetFrameStatistics(stats);
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override { return state.swapchain->GetLastPresentCount(count); }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return state.swapchain->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override { return state.swapchain->GetRestrictToOutput(output); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override { return state.swapchain->SetBackgroundColor(color); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override { return state.swapchain->GetBackgroundColor(color); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override { return state.swapchain->SetRotation(rotation); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override { return state.swapchain->GetRotation(rotation); }
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override { return state.swapchain->SetSourceSize(w, h); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override { return state.swapchain->GetSourceSize(w, h); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override { return state.swapchain->SetMaximumFrameLatency(latency); }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* latency) override { return state.swapchain->GetMaximumFrameLatency(latency); }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return state.swapchain->GetFrameLatencyWaitableObject(); }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* m) override { return state.swapchain->SetMatrixTransform(m); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m) override { return state.swapchain->GetMatrixTransform(m); }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return 0; }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE color, UINT* support) override {
        return state.swapchain->CheckColorSpaceSupport(color, support);
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE color) override { return state.swapchain->SetColorSpace1(color); }
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* data) override {
        return state.swapchain->SetHDRMetaData(type, size, data);
    }
};
}

HRESULT CreateNativeD3D11Proxy(IDXGIFactory2* factory, ID3D11Device* device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1& desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGISwapChain1** chain) {
    if(!chain || desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.SampleDesc.Count != 1 || desc.Stereo
        || !(desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT)
        || (desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD && desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL))
        return DXGI_ERROR_UNSUPPORTED;
    auto* proxy = new(std::nothrow) NativeSwapchain(device, desc);
    if(!proxy) return E_OUTOFMEMORY;
    const HRESULT hr = proxy->Initialize(factory, window, fullscreen);
    if(FAILED(hr)) proxy->Release();
    else *chain = proxy;
    return hr;
}
