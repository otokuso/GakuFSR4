# FG backbuffer formats and HDR (#10)

The existing manual FG path copies resources between the game's D3D11 backbuffer and a separate D3D12 FG backend. It currently assumes `DXGI_FORMAT_R8G8B8A8_UNORM` and the SDR `RGB_FULL_G22_NONE_P709` color space. The FG context's declared `backBufferFormat`, output UAVs, source copies, generation transfer function and final UI composition are all built around this assumption.

The compatibility diagnostics now report the actual backbuffer format by name, color space enum, dimensions and sample count when FG cannot run:

| Backbuffer | Current manual FG path | Work needed |
| --- | --- | --- |
| RGBA8 SDR | Supported | Verify Present1, resize and alpha/UI handling |
| BGRA8 SDR | Unsupported | Verify D3D11/D3D12 typed UAV support or add BGRA↔RGBA conversion during copy/composite |
| RGB10A2 / HDR10 PQ | Unsupported | Preserve HDR transfer function and luminance metadata through FG dispatch |
| RGBA16F / scRGB HDR | Unsupported | Native float output, appropriate transfer function and HDR UI compositing |

Do not accept a format merely because the FFX enum defines it. `CopyResource` requires format compatibility and the downstream output/UAV and Unity buffer formats must agree. The native AMD FG SwapChain proof of concept (#23) is the preferred place to explore non-RGBA8 presentation once its D3D11 interoperability works.

Record `GetColorSpace1`, format and monitor HDR state in SDR, Windows Auto HDR/scRGB and HDR10 modes. Until those cases are tested on the target display/GPU, skip FG safely and keep the real game frame visible.
