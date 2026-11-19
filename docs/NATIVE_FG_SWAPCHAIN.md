# AMD Frame Generation SwapChain proof of concept (#23)

The native D3D12 AMD SwapChain is a separate technology from the existing manual FSR FG dispatch + two Present calls. The current game is still rendered with Direct3D 11, while the AMD API requires a Direct3D 12 command queue as its presentation owner.

`test_native_swapchain` is an **isolated Windows-only experiment**. It does not inject into GakuLoader or replace the Unity swapchain. The program:

1. Creates D3D11 and D3D12 devices on the same hardware adapter.
2. Creates an AMD Frame Generation SwapChain directly for a dedicated test HWND using the official `ffxCreateContextDescFrameGenerationSwapChainForHwndDX12` descriptor, version 4.0.0.
3. Clears a D3D11 RGBA8 render texture, shares it with D3D12 through a named handle, and orders GPU access using a shared fence.
4. Copies a moving test pattern from the shared texture to the AMD SwapChain. The optional `--fg` mode also prepares flat depth and zero motion vectors on D3D12 and registers a generation callback with the SwapChain. The callback dispatches interpolation during presentation.

Run on Windows 11 with the official signed AMD SDK v2.4.0 runtime DLLs:

```powershell
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
cmake --build build --config Release --target test_native_swapchain
.\build\Release\test_native_swapchain.exe C:\AMD-FSR-runtime
.\build\Release\test_native_swapchain.exe C:\AMD-FSR-runtime --fg
.\build\Release\test_native_swapchain.exe C:\AMD-FSR-runtime --fg --borderless
```

The test submits 480 source frames at a target of 60 FPS. `--borderless` uses the monitor containing the cursor, with a native-size backbuffer and the DXGI tearing flag. Run PresentMon 2.x with `--process_name test_native_swapchain.exe --track_frame_type` before launching the test. With SDK 2.4.0, PresentMon labels the AMD generated frames `Unknown`. Check the display intervals and compare frame-type counts with FG off. The synthetic motion and depth make this a presentation test. It requires AMD hardware for the AMD FG provider.

## Game integration

The ordinary Unity D3D11 flip-model swapchain and an AMD D3D12 flip-model swapchain cannot own the same HWND at the same time. The current `Present` hook runs after Unity has created its swapchain. A prototype that keeps native D3D11 rendering therefore needs to proxy swapchain creation and expose D3D11-compatible backbuffers via `GetBuffer`. On `Present`, it can submit the shared colour and FG inputs to D3D12 as this test does. On `ResizeBuffers` or device loss it must drain both queues and the AMD SwapChain before replacing those buffers.

Select that path before creating the game's swapchain. If AMD context setup fails during startup, release its HWND ownership before creating Unity's ordinary swapchain and using existing manual FG. An in-session switch needs to rebuild the swapchain and its dependent Unity resources. D3D11On12 is a separate candidate and needs to establish that Unity's device and resource usage work through the translation layer. Keep the manual path available while testing these alternatives.

Remaining checks include DAFP activation, UI composition, HDR, device loss, fullscreen transitions, Alt+Tab, and a matched comparison against manual FG for pacing, input latency, CPU, VRAM and stability.

## Hardware result, 2026-11-19

On an RX 9070 XT with Adrenalin 26.11.1 (32.0.32015.16001), Windows 11 build 26300, and the SDK v2.4.0 runtime, both FG runs dispatched 480 interpolated frames. In the middle 6.5 seconds of each PresentMon 2.6.0 capture:

| Mode | FG | Display FPS | p99 display interval | Application / Unknown display intervals |
| --- | --- | ---: | ---: | ---: |
| 960×540 windowed | Off | 59.63 | 31.26 ms | 388 / 0 |
| 960×540 windowed | On | 119.96 | 25.00 ms | 390 / 389 |
| 3840×2160 borderless | Off | 58.71 | 31.27 ms | 382 / 0 |
| 3840×2160 borderless | On | 119.84 | 25.00 ms | 390 / 389 |

All rows used Composed Flip. The borderless run requested tearing with VSync off, but PresentMon reported `AllowsTearing=0`. A repeat with the borderless window promoted to topmost showed the same mode and flag at 119.96 displayed FPS. This confirms display of interpolated frames. Driver-assisted pacing and VRR operation still need confirmation on a qualifying display path.

References: [AMD SwapChain integration and DAFP prerequisites](https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-swap-chain/) · [DXGI flip-model swapchain creation](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd)
