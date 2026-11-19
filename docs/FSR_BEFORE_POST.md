# Optional NativeAA before post-processing (issue #26)

The screen-camera route normally copies TAA inputs forward and dispatches FSR from `PostProcessPass.RenderFinalPass`, after URP's normal post-processing stages. The `NativeAABeforePost` switch moves **NativeAA (render scale 1.0 only)** to the TAA hook, using the same already-sized source/destination RTHandles, so later URP post passes process its output. No intermediate post-process buffers are reallocated.

Add to `Configs/gakufsr4.ini`:

```ini
[FSR4]
Preset=NativeAA
NativeAABeforePost=1
```

Default is `0`. This mode applies only to the screen camera, with `Ratio=1` or `Preset=NativeAA`; offscreen cameras keep the existing NativeAA path. At other ratios the plugin logs a warning and continues using the current final-pass route. Frame validation/fallback and camera-reset tracking remain enabled, and successful early dispatch suppresses the duplicate dispatch in `RenderFinalPass`.

The setting is also available under **Native AA / jitter (experimental)** in GakuLoader's F1 panel. The setting is saved to the INI and takes effect after restarting the game. The panel shows the active dispatch stage and queued frames to distinguish a selected option from one actually being used.

This is an **experimental placement alternative**, not a promise of better image quality. With NativeAA, render and output resolution are already identical, so the post-process resolution does not change; only the order of FSR AA and post effects changes. For upscaling ratios above 1, the current TAA destination and later post passes are still allocated at render resolution. Redirecting them safely to a display-resolution target requires further changes to URP's RTHandle allocation, intermediate passes, camera stacking and final blit/resolve logic.

In the cited Unity URP 2022.3 path, depth of field is applied **before** `TemporalAA.ExecutePass`; bloom and tonemapping occur later. Thus even `NativeAABeforePost=1` does not move every effect to AMD's recommended post-FSR stage. The actual game's URP version and custom passes still need GPU-frame inspection.

## Windows/Unity comparison

1. Capture identical scenes with `NativeAABeforePost=0` and `=1` at the same resolution, graphics settings and camera motion, with FG initially disabled.
2. Verify the original TAA fallback after injection failures, NativeAA offline RenderTextures, scene camera/overlay order, bloom edges, DOF, motion blur, tone mapping, and HDR-on/off behavior.
3. Record FSR GPU times, total frame times and screenshots; then repeat with FG. Disable the mode if a post-processing pass assumes a TAA history texture not supplied by FSR.
4. Treat a ratio >1 relocation as a separate renderer-architecture change; it cannot be validated by the present 1:1-mode tests.

References:

- AMD FSR SDK, frame placement: https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-ml/#placement-in-the-frame
- Unity URP 2022.3 `PostProcessPass.cs`: https://github.com/Unity-Technologies/Graphics/blob/2022.3/staging/Packages/com.unity.render-pipelines.universal/Runtime/Passes/PostProcessPass.cs
