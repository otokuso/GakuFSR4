# FG backbuffer formats and HDR (#10)

The manual FG path copies resources between the game's D3D11 backbuffer and a separate D3D12 FG backend. It supports single-sample RGBA8 SDR and RGBA16F scRGB using the DXGI default color space for each. It uses the backbuffer format for the FG context and scaling textures, and passes the corresponding transfer function to the SDK. A format change rebuilds the FG context before interpolation resumes. scRGB dispatch uses the display luminance reported by DXGI, with a 0–1000 nit fallback.

The compatibility diagnostics report the backbuffer format by name, dimensions and sample count when FG cannot run. The format check rejects BGRA8 and RGB10A2.

| Backbuffer | Current manual FG path | Work needed |
| --- | --- | --- |
| RGBA8 SDR | Supported | Verify Present1, resize and alpha/UI handling |
| BGRA8 SDR | Real-frame fallback | A D3D11 shader round trip is verified. Adoption needs state-safe conversion around the bridge and full FG testing |
| RGB10A2 / HDR10 PQ | Real-frame fallback | Keep the correct PQ transfer, luminance in nits and swap-chain color space through FG and UI composition |
| RGBA16F / scRGB HDR | Supported | Validate HDR appearance in game with the chosen ReShade effects |

`CopyResource` requires compatible formats across the backbuffer, shared textures, and FG output. The manual path keeps FP16 data through the scaling and presentation copies.

For HDR testing, capture the backbuffer format, successful swap-chain color-space changes and monitor HDR state in SDR, Windows Auto HDR/scRGB and HDR10 modes. Verify that unsupported formats keep the real game frame visible.

## Hardware verification

For ReShade with an FP16 backbuffer, use `FrameGeneration=1` and `NativeSwapChain=0`. The manual path presents through the game's swap chain. The optional native path remains tracked in #23 and UI composition in #9.

On 2026-11-19 the game logged format 28 (RGBA8 UNORM) at 1920×1080 and 3840×2160. On Windows 11 build 26300 and Radeon RX 9070 XT (driver 32.0.32015.16001), the stand-alone harness verified:

- BGRA8 shader conversion to RGBA8 and back preserved all four channels, including alpha, through GPU readback.
- BGRA8 and RGB10A2 swap chains displayed all five real Presents with zero generated frames. The log named each format and the settings panel's diagnostic source reported the incompatible-backbuffer reason.
- The usual RGBA8 scenario continued generating frames with FG 4.0.2.

Use the AMD SDK v2.4.0 test runtime and run the GPU harness with `--fg-scrgb` to verify FP16 interpolation on a smaller swap chain through both `Present` and `Present1`. Use `--fg-bgra` and `--fg-rgb10` to verify real-frame fallback.

References: [AMD FG input color requirements](https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-api/#input-color-requirements), [DXGI swap-chain color space](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-setcolorspace1).
