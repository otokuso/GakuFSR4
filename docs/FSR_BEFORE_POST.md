# NativeAA before post-processing (issue #26)

Screen-camera NativeAA runs in the TAA pass at render scale 1.0 (`Preset=NativeAA` or legacy `Ratio=1`). It uses the existing output-sized source and destination RTHandles. Successful early dispatch suppresses duplicate dispatch in `RenderFinalPass`. The legacy `NativeAABeforePost` setting is ignored.

Upscaling presets dispatch FSR from `PostProcessPass.RenderFinalPass` because the TAA destination and intermediate post targets are allocated at render resolution. Offscreen NativeAA continues to run in the TAA pass. Frame validation, game TAA fallback, and camera history resets apply to the early path.

In the cited Unity URP 2022.3 path, depth of field runs before `TemporalAA.ExecutePass`. Bloom and tone mapping run later. The game's custom passes still need GPU-frame inspection.

## Windows and Unity verification

1. Start with `Preset=NativeAA` and check for `Queued first screen NativeAA frame before post-processing` in `gakuloader.log` and the early queue counter in GakuLoader's runtime diagnostics.
2. Verify game TAA fallback, offscreen RenderTextures, bloom, depth of field, motion blur, tone mapping, and HDR. Repeat with FG enabled.
3. Test an upscaling preset and verify the final-pass output resolution and post-processing.

References

- [AMD FSR SDK frame placement](https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-ml/#placement-in-the-frame)
- [Unity URP 2022.3 PostProcessPass.cs](https://github.com/Unity-Technologies/Graphics/blob/2022.3/staging/Packages/com.unity.render-pipelines.universal/Runtime/Passes/PostProcessPass.cs)
