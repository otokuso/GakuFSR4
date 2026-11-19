#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>

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
HRESULT CreateNativeD3D11Proxy(IDXGIFactory2*, ID3D11Device*, HWND, const DXGI_SWAP_CHAIN_DESC1&,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGISwapChain1**);
