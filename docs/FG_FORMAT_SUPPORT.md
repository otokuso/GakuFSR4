# FG backbuffer formats and HDR (#10)

The AMD Native SwapChain supports single-sample flip-model backbuffers. For RGBA8, the D3D11 proxy exposes a typeless texture so Unity can create an sRGB render-target view.

| Backbuffer | Presentation |
| --- | --- |
| RGBA8 SDR, RGBA16F scRGB, RGB10A2 HDR10 | AMD Native SwapChain with FG when enabled and supported |
| BGRA8 SDR | Game swap chain with real frames and FSR upscaling |

ReShade applies effects to the game's D3D11 backbuffer before it is copied to the private AMD D3D12 SwapChain. When AutoHDR changes formats, the proxy resizes both buffers and rebuilds FG with the new format. The AMD callback supplies the transfer function and display luminance.

AutoHDR 2024.4.17 shares its HDR settings across ReShade runtimes in the same process. If GakuLoader's desktop window gets `ReShade.ini` and Unity gets `ReShade2.ini`, set the same `[HDR]` values in both files for consistent startup.

References: [AMD FG input color requirements](https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-api/#input-color-requirements), [DXGI swap-chain color space](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-setcolorspace1).
