# AMD Frame Generation SwapChain proof of concept (#23)

The native D3D12 AMD SwapChain is a separate technology from the existing manual FSR FG dispatch + two Present calls. The current game is still rendered with Direct3D 11, while the AMD API requires a Direct3D 12 command queue as its presentation owner.

`test_native_swapchain` is an **isolated Windows-only experiment**. It does not inject into GakuLoader or replace the Unity swapchain. The program:

1. Creates D3D11 and D3D12 devices on the same hardware adapter.
2. Creates an AMD Frame Generation SwapChain directly for a dedicated test HWND using the official `ffxCreateContextDescFrameGenerationSwapChainForHwndDX12` descriptor, version 4.0.0.
3. Clears a D3D11 RGBA8 render texture, shares it with D3D12 through a named handle, and orders GPU access using a shared fence.
4. Copies the shared texture into a native AMD SwapChain backbuffer, then calls its `Present`.

Run on Windows 11 with the official signed AMD SDK v2.4.0 runtime DLLs:

```powershell
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
cmake --build build --config Release --target test_native_swapchain
.\build\Release\test_native_swapchain.exe C:\AMD-FSR-runtime
```

Record the exact SDK return code, adapter, driver, OS build, whether the green test window appears, and the present/fence results. This requires real hardware; a hosted CI VM cannot validate it.

**Still required before integrating into the game:** intercept or proxy the real D3D11 swapchain's `GetBuffer`, `Present` and `ResizeBuffers`; preserve Unity's backbuffer ownership semantics; register FG prepare/interpolation callbacks; verify generated frames and driver-assisted frame pacing; handle UI, HDR, device loss, fullscreen and Alt+Tab; compare frame pacing and latency with the existing manual Present fallback. Do not switch the game path merely because this separate test succeeds.
