#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// ReShade 6.8's IID_UnwrappedObject returns the native COM interface.
template<class T>
Microsoft::WRL::ComPtr<T> UnwrapReShade(T* object)
{
    constexpr GUID iid = {0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
    Microsoft::WRL::ComPtr<T> original;
    if (FAILED(object->QueryInterface(iid, reinterpret_cast<void**>(original.GetAddressOf()))))
        original = object;
    return original;
}

// The proxy presents the application's native D3D11 render target through the AMD D3D12 chain.
struct NativeSwapchainState {
    void* context = nullptr;
    IDXGISwapChain4* swapchain = nullptr;
};

HRESULT CreateNativeSwapchain(IDXGIFactory2*, ID3D11Device*, HWND, DXGI_SWAP_CHAIN_DESC1&,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, NativeSwapchainState&);
void DestroyNativeSwapchain(NativeSwapchainState&);
HRESULT PresentNativeSwapchain(NativeSwapchainState&, ID3D11Texture2D*, UINT, UINT,
    const DXGI_PRESENT_PARAMETERS*);
HRESULT ResizeNativeSwapchain(NativeSwapchainState&, UINT, UINT, UINT, DXGI_FORMAT, UINT);
HRESULT SetNativeSwapchainColorSpace(NativeSwapchainState&, DXGI_COLOR_SPACE_TYPE);
HRESULT CreateNativeD3D11Proxy(IDXGIFactory2*, ID3D11Device*, HWND, const DXGI_SWAP_CHAIN_DESC1&,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGISwapChain1**);
