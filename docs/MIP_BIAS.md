# Mip LOD bias investigation (#7)

AMD recommends `mipBias = log2(renderWidth / displayWidth) - 1` for FSR upscaling. This produces approximately -1.585 (Quality), -1.766 (Balanced), -2.0 (Performance), and -2.585 (Ultra Performance). A more negative bias favors more detailed mip levels but can also increase aliasing.

The game uses Unity URP, whose shaders can incorporate the `_GlobalMipBias` uniform when image scaling is active. The plugin already modifies URP's render scale and upscaling mode; URP **may** already apply some or all of this adjustment. Setting the AMD-recommended bias again without inspecting the current value risks double biasing.

The current change computes and logs the AMD reference value once, but intentionally does not alter texture samplers or global shader state. On the game's supported Radeon build, inspect the `_GlobalMipBias` value and actual mip selection in a graphics capture at NativeAA, Quality, Balanced and Performance before deciding whether to inject a corrected value. If an override is needed, apply it only to the target camera and restore the previous value when the camera ends.

Primary sources: [AMD FSR4 mipmap biasing](https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-ml/#mipmap-biasing) and [Unity URP Core.hlsl global mip bias sampling](https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl).
