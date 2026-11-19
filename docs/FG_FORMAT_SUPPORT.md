# FG backbuffer formats and HDR (#10)

The existing manual FG path copies resources between the game's D3D11 backbuffer and a separate D3D12 FG backend. It currently assumes `DXGI_FORMAT_R8G8B8A8_UNORM` and the SDR `RGB_FULL_G22_NONE_P709` color space. The FG context's declared `backBufferFormat`, output UAVs, source copies, generation transfer function and final UI composition are all built around this assumption.

The compatibility diagnostics report the backbuffer format by name, dimensions and sample count when FG cannot run. The format check rejects BGRA8, RGB10A2 and RGBA16F. It cannot determine the active color space of an RGBA8 swap chain.

| Backbuffer | Current manual FG path | Work needed |
| --- | --- | --- |
| RGBA8 SDR | Supported | Verify Present1, resize and alpha/UI handling |
| BGRA8 SDR | Real-frame fallback | A D3D11 shader round trip is verified. Adoption needs state-safe conversion around the bridge and full FG testing |
| RGB10A2 / HDR10 PQ | Real-frame fallback | Keep the correct PQ transfer, luminance in nits and swap-chain color space through FG and UI composition |
| RGBA16F / scRGB HDR | Real-frame fallback | Keep linear float output and composite UI in the same color space |

Do not accept a format merely because the FFX enum defines it. `CopyResource` requires format compatibility and the downstream output/UAV and Unity buffer formats must agree. The native AMD FG SwapChain proof of concept (#23) is the preferred place to explore non-RGBA8 presentation once its D3D11 interoperability works.

For HDR testing, capture the backbuffer format, successful swap-chain color-space changes and monitor HDR state in SDR, Windows Auto HDR/scRGB and HDR10 modes. Verify that unsupported formats keep the real game frame visible.

## Decision and hardware verification

Keep the manual path RGBA8 SDR-only. Use a format-aware draw, rather than a raw CopyResource, if BGRA8 conversion is integrated later. Defer manual HDR support until the bridge can propagate the actual swap-chain color space and luminance and verify HDR UI composition. Neither RGB10A2 storage nor an HDR desktop mode establishes the encoding of the game's backbuffer. The optional native path remains tracked separately in #23 and UI composition in #9.

On 2026-11-19 the game logged format 28 (RGBA8 UNORM) at 1920×1080 and 3840×2160. On Windows 11 build 26300 and Radeon RX 9070 XT (driver 32.0.32015.16001), the stand-alone harness verified:

- BGRA8 shader conversion to RGBA8 and back preserved all four channels, including alpha, through GPU readback.
- BGRA8 and RGB10A2 swap chains displayed all five real Presents with zero generated frames. The log named each format and the settings panel's diagnostic source reported the incompatible-backbuffer reason.
- The usual RGBA8 scenario continued generating frames with FG 4.0.2.

Use the regular AMD SDK v2.4.0 test runtime and run the existing GPU harness with --fg-bgra and --fg-rgb10. The RGB10A2 case checks fallback for the storage format. Run separate PQ/scRGB and HDR UI checks before implementing HDR interpolation.

References: [AMD FG input color requirements](https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-api/#input-color-requirements), [DXGI swap-chain color space](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-setcolorspace1).