#pragma once
#include <cmath>
#include <cstdint>

struct TaaMatrixOffset { float x, y; };

inline int FsrRecommendedJitterPhases(float upscaleRatio)
{
    if (!std::isfinite(upscaleRatio) || upscaleRatio < 1 || upscaleRatio > 3) return 8;
    return static_cast<int>(std::round(8.0f * upscaleRatio * upscaleRatio));
}

inline float TaaHalton(uint32_t index, uint32_t base)
{
    float value = 0, fraction = 1;
    while (index) {
        fraction /= base;
        value += fraction * (index % base);
        index /= base;
    }
    return value;
}

inline TaaMatrixOffset RepairTaaJitter(float oldX, float oldY, int originalW, int originalH,
                                       int renderW, int renderH, int frame, int phaseOffset, float jitterScale,
                                       int phaseCount = 1024, bool overrideUnityJitter = false)
{
    if (originalW <= 0 || originalH <= 0 || renderW <= 0 || renderH <= 0) return {oldX, oldY};
    if (!overrideUnityJitter && std::isfinite(oldX) && std::isfinite(oldY) && (std::fabs(oldX) > 1e-8f || std::fabs(oldY) > 1e-8f))
        return { oldX * originalW / renderW, oldY * originalH / renderH };
    // URP had TAA off when it initialized the camera: regenerate the normal
    // Halton 2/3 sequence after enabling TAA and scaling the target descriptor.
    if (phaseCount < 1 || phaseCount > 1024) phaseCount = 1024;
    const uint32_t phase = (static_cast<uint32_t>(frame) + static_cast<uint32_t>(phaseOffset)) % static_cast<uint32_t>(phaseCount);
    const float scale = std::isfinite(jitterScale) && jitterScale >= 0 && jitterScale <= 2 ? jitterScale : 1;
    return { (TaaHalton(phase + 1, 2) - 0.5f) * scale * 2 / renderW,
             (TaaHalton(phase + 1, 3) - 0.5f) * scale * 2 / renderH };
}
