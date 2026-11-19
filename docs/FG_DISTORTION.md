# FG lens-distortion field (issue #19)

FG's Prepare pass receives undistorted scene depth and motion vectors, whereas the displayed backbuffer may contain post-process lens distortion. The FidelityFX Frame Generation API supports an optional RG distortion field whose two channels encode `distortedUV - originalUV` at each pixel.

This patch adds an **opt-in, static radial UV-field approximation**. If any coefficient is nonzero, the bridge generates one `R32G32_FLOAT` texture at the FG display resolution, uploads it on the D3D12 graphics command list, and links it to the optional `ffxConfigureDescFrameGenerationRegisterDistortionFieldResource` descriptor. If the provider rejects that extension, the bridge logs the failure and retries normal FG. Zero coefficients (the default) allocate nothing and leave existing FG behavior unchanged.

In `Configs/gakufsr4.ini`:

```ini
[FSR4]
FGDistortionK1=0
FGDistortionK2=0
FGDistortionCenterX=0.5
FGDistortionCenterY=0.5
```

For normalized screen UV `u,v`, `delta=(u-centerX, v-centerY)` and `r2=dot(delta,delta)`, each field pixel contains:

```
distortedUV - originalUV = delta * (K1*r2 + K2*r2*r2)
```

This is intentionally *not* a direct translation of URP's Lens Distortion component, which can have different intensity, x/y multipliers, scale, centers, dynamic values and rendering order. Nonzero values must be derived from a real before/after distortion capture; a guessed coefficient can make FG look worse. The optional texture is capped at a 3840×2160 pixel count to bound persistent default/upload memory.

## Remaining verification

- Determine whether the game's URP camera actually uses post-process distortion in the problem scenes. If not, leave both coefficients at zero.
- If distortion is dynamic or non-radial, replace the static approximation with the *actual per-frame 2D UV delta map*, maintaining the same FFX resource-registration path.
- On Windows/AMD, compare FG on/off with a known post-process distortion scene and inspect the field orientation/sign, GPU errors, resource lifetime after resize, and input/output frame parity. The math test checks the radial coordinate calculation. Run the standalone GPU harness as described in GPU_TESTING.md with --fg-distortion to check upload and FG dispatch with a synthetic field.

Reference: https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-api/#distortion-field
