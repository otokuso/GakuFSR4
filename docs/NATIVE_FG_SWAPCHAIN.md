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
.\build\Release\test_native_swapchain.exe C:\AMD-FSR-runtime --fg --borderless --on12
```

The test submits 480 source frames at a target of 60 FPS. `--borderless` uses the monitor containing the cursor, with a native-size backbuffer and the DXGI tearing flag. Run PresentMon 2.x with `--process_name test_native_swapchain.exe --track_frame_type` before launching the test. With SDK 2.4.0, PresentMon labels the AMD generated frames `Unknown`. Check the display intervals and compare frame-type counts with FG off. The synthetic motion and depth make this a presentation test. It requires AMD hardware for the AMD FG provider.

`--on12` renders directly to D3D11On12 wrappers around the AMD SwapChain backbuffers. The D3D11 immediate context flushes to the same D3D12 queue before the SDK prepares and presents each frame. This path eliminates the shared-colour copy and the cross-device fence wait from the test. Both paths retain the per-frame completion fence for allocator reuse.

## Game integration

The opt-in game path uses `FrameGeneration=1` and `NativeSwapChain=1` at startup. The Unity factory hook creates an AMD-owned D3D12 SwapChain and exposes an RGBA8 D3D11 texture at buffer index 0. The typeless texture allows Unity to create an sRGB render-target view. The existing bridge supplies depth and motion to the AMD callback and copies the final composited backbuffer at Present. A resize drains outstanding native work before replacing the buffer and resetting FG history.

Unsupported startup descriptors or SDK startup failures use the existing manual path. After a native chain has been created, restart with `NativeSwapChain=0` to switch back. This path keeps native D3D11 rendering. It uses the final composited colour, including the game's UI, as the FG source. It currently supports single-sample RGBA8 SDR. Device-loss recovery and live HDR transitions still need validation.

The game launched the AMD SwapChain 4.0.0 and FG 4.0.2 on the RX 9070 XT in Quality mode. The title screen displayed correctly after allowing the sRGB view, and the game recreated the buffers on transition from portrait to fullscreen landscape. A ten-second fullscreen live capture showed 595 displayed application intervals and 595 AMD-generated intervals with SyncInterval 0, at about 120 displayed FPS. The native chain used Composed Flip in that capture. A later capture contained 21 independent-flip intervals with tearing allowed, but most frames still used Composed Flip. The public AMD SDK APIs used here do not report whether the driver selected DAFP. The capture proves actual in-game interpolation and records the observed presentation modes.

## Hardware result, 2026-11-19

On an RX 9070 XT with Adrenalin 26.11.1 (32.0.32015.16001), Windows 11 build 26300, and the SDK v2.4.0 runtime, both FG runs dispatched 480 interpolated frames. In the middle 6.5 seconds of each PresentMon 2.6.0 capture:

| Mode | FG | Display FPS | p99 display interval | Application / Unknown display intervals |
| --- | --- | ---: | ---: | ---: |
| 960×540 windowed | Off | 59.63 | 31.26 ms | 388 / 0 |
| 960×540 windowed | On | 119.96 | 25.00 ms | 390 / 389 |
| 3840×2160 borderless | Off | 58.71 | 31.27 ms | 382 / 0 |
| 3840×2160 borderless | On | 119.84 | 25.00 ms | 390 / 389 |

All rows used Composed Flip. The borderless run requested tearing with VSync off, but PresentMon reported `AllowsTearing=0`. A repeat with the borderless window promoted to topmost showed the same mode and flag at 119.96 displayed FPS. This confirms display of interpolated frames. Driver-assisted pacing and VRR operation still need confirmation on a qualifying display path.

## D3D11On12 comparison

The same Radeon system ran both colour paths at 3840×2160 borderless with FG enabled and SyncInterval 0. Both dispatched 480 interpolated frames and showed 390 application and 390 AMD-generated display intervals in the middle 6.5 seconds. Both traces reported `Hardware: Independent Flip` and `AllowsTearing=1` for all those intervals.

| Colour path | Display FPS | p99 display interval | Process CPU time | Peak sampled dedicated VRAM |
| --- | ---: | ---: | ---: | ---: |
| Native D3D11, shared texture | 120.04 | 22.08 ms | 1.48 s | 573.6 MiB |
| D3D11On12, wrapped backbuffers | 119.98 | 22.26 ms | 0.97 s | 559.2 MiB |

Each row is a single eight-second synthetic run. CPU time is for the whole process. VRAM is the maximum of the per-process dedicated usage sampled about once per second. The run confirms independent presentation with tearing allowed. It provides no direct measurement of the active AMD pacing mechanism or the monitor's VRR refresh. The existing manual game path and input-to-display latency still need a matched comparison. D3D11On12 compatibility with Unity's full renderer also remains open.

References: [AMD SwapChain integration and DAFP prerequisites](https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-swap-chain/) · [DXGI flip-model swapchain creation](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd) · [Direct3D 11 on 12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-11-on-12)
