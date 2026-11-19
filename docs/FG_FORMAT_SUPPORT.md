# FG backbuffer formats and HDR (#10)

The AMD Native SwapChain supports a single-sample RGBA8 SDR backbuffer and a flip-model swap effect. The D3D11 proxy exposes an RGBA8 typeless texture so Unity can create an sRGB render-target view.

| Backbuffer at startup | Presentation |
| --- | --- |
| RGBA8 SDR | AMD Native SwapChain with FG when enabled and supported |
| BGRA8 SDR, RGB10A2 HDR10, RGBA16F scRGB | Game swap chain with real frames and FSR upscaling |

ReShade AutoHDR can request an RGBA16F backbuffer, which uses the real-frame fallback. For HDR testing, record the backbuffer format, swap-chain color space and monitor HDR state, and verify that real frames stay visible. Live HDR transitions still need game validation.

References: [AMD FG input color requirements](https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-api/#input-color-requirements), [DXGI swap-chain color space](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-setcolorspace1).
