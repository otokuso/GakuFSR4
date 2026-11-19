# Native AA validation checklist

`Preset=NativeAA` uses full-resolution color/depth/motion-vector inputs and
dispatches the FSR upscaler at a 1:1 render-to-display ratio. It does **not**
remove the cost of native-resolution game rendering. The screen camera uses
the TAA pass. The jitter phase modes below are available for comparison.

## Reproducible A/B matrix

Start with `FrameGeneration=0` and `Sharpness=0` at the same resolution, camera
angle, game graphics settings, HDR state and provider version. Restart the game
after changing an option.

| Run | `JitterPhaseMode` | Meaning |
| --- | --- | --- |
| A (baseline) | `0` | Unity 1024-phase Halton |
| B | `2` | FSR 8-phase Native AA jitter |

`JitterPhaseMode=1` uses the FSR ratio-dependent sequence for **all** presets;
mode `2` changes the sequence only at ratio 1.0. Both are opt-in. The actual
jitter applied to a frame and the value handed to the SDK must agree; the UI
shows the *submitted* pixel offsets, not a GPU capture of camera projection.

For each run, capture the same live-scene segment:

1. Pause/steady camera: hair strands, costume edges, stage LEDs and fine text.
2. Slow pan: edge shimmer, ghost trails, alpha-blended effects.
3. Fast cut: first frame after a camera or scene transition, history reset count.
4. Bright bloom, depth of field and rapid motion: check output with each jitter mode.
5. Repeat with FG enabled and compare real versus generated frames independently.

Use the GakuLoader panel to check **Native AA stage**, **Jitter phases**,
**Native AA queued**, **Last jitter**, **input HDR flag** and **History resets**.
The counts are CPU command-queue observations. `FSR frames` and backend errors
give additional runtime evidence; neither proves that input pixels and motion
vectors are correct.

## GPU validation still required

- Capture the actual color, depth and motion-vector textures at dispatch. Check
  their dimensions, the motion-vector sign/convention, jitter cancellation and
  the source-to-output timing. The plugin declines early/offscreen Native AA if
  either TAA RTHandle's texture dimensions differ from the camera descriptor;
  this is a safe game-TAA fallback, not proof that the FSR path is correct.
- Compare the linear/non-linear color space at the FSR input for SDR and HDR
  output. `input HDR` in the panel reports the **FSR create flag**, not a
  measurement of color-buffer transfer function or dynamic range.
- Unity's own TAA persistent targets may still be allocated while the TAA pass
  is intercepted. Do not bypass target allocation without verifying original
  TAA fallback, camera stacks and offscreen RenderTextures.
- The manual FG Present path has no validated independent UI compositor.
  Early Native AA intentionally does not expose its pre-post-process result as
  the HUD-less final-scene diagnostic; see [FG UI composition](FG_UI_COMPOSITION.md).
- A scene cut that changes content without changing camera identity, transform,
  FOV or frame continuity may not be detected automatically. The panel has a
  **Reset Native AA history on next frame** button for testing these cases.

## References

- [AMD FSR 4 integration: placement, motion vectors, jitter, exposure and HDR](https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-ml/)
- [Unity URP 2022.3 PostProcessPass](https://github.com/Unity-Technologies/Graphics/blob/2022.3/staging/Packages/com.unity.render-pipelines.universal/Runtime/Passes/PostProcessPass.cs)
- [Unity URP 2022.3 TemporalAA](https://github.com/Unity-Technologies/Graphics/blob/2022.3/staging/Packages/com.unity.render-pipelines.universal/Runtime/TemporalAA.cs)
- [GakuFSR4 Native AA interception](../src/GakuFSR4.cpp)
