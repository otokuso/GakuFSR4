# FG scene / UI composition — #9

The scene render target created by the screen-camera `RenderFinalPass` hook is available before the game's final swap-chain presentation. When frame generation is enabled, the bridge now labels this FSR output as a **candidate HUD-less image** and exposes its D3D11 shader-resource view via `FSR4_GetHudlessView()`. The non-null view shares the same generation and view-lifetime rules as `FSR4_GetViews()`. `fgHudless` counts successful captures, **not** frames with separated or composited UI.

This image is not necessarily UI-free. URP can draw world-space or screen-space camera UI before this hook; overlay canvases and native desktop overlays can be drawn later. The game must be checked per scene and per UI layer before labeling it HUD-less.

## Validation procedure on a game PC

1. Capture the candidate HUD-less render target and the actual backbuffer on the same real frame, without FG. Compare HUD text, idol gauges, pause controls, live subtitles, and transparent elements in normal and rotated gameplay.
2. If the images differ only by known UI layers, obtain the original *premultiplied/straight-alpha UI texture* or a deterministic post-FG UI redraw path, and verify the correct alpha-blend mode. Blindly subtracting the backbuffer and HUD-less colors will not reconstruct UI alpha for arbitrary opaque/transparent pixels.
3. For AMD's SwapChain-based path, pass the pre-UI buffer to the frame-generation configuration and the independent UI layer to the swap chain's UI resource registration, and test both real and generated frames.
4. Capture a frame pair with FG off/on. Compare thin text, fast-moving note lanes, gauges, color conversion, letterbox edges, and action buttons, then check behavior at resize and camera cuts.

**Current limit:** The bridge exposes a debug/capture surface. FG currently receives the final frame with UI. Independent UI composition requires verified Unity canvas/pass ordering and a separate UI layer.

References:

- AMD FidelityFX Frame Interpolation Swap Chain: https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-swap-chain/
- FFX SDK UI composition and HUDLessColor fields: `vendor/ghpc/ffx/Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h`
