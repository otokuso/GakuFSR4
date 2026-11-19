#pragma once
#include <cmath>

// AMD's recommended starting point for upscaled texture sampling:
// log2(renderResolution / displayResolution) - 1.
inline float RecommendedFsrMipBias(float upscaleRatio)
{
    if (!std::isfinite(upscaleRatio) || upscaleRatio <= 1) return 0.0f;
    return -std::log2(upscaleRatio) - 1.0f;
}
