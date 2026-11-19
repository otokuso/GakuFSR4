#pragma once
#include <cmath>
#include <cstdint>

struct TaaMatrixOffset { float x, y; };

// NativeAA defaults to the FSR 1:1 sequence (8 phases). Upscaling presets
// retain the original Unity 1024-phase behavior unless explicitly overridden.
constexpr int DefaultJitterPhaseMode = 2;

inline int FsrRecommendedJitterPhases(float ratio)
{
    return std::isfinite(ratio) && ratio>=1 && ratio<=3 ? static_cast<int>(std::round(8*ratio*ratio)) : 8;
}

// 0: Unity 1024-frame Halton sequence (legacy), 1: FSR ratio-dependent sequence,
// 2: FSR sequence for NativeAA only (default).
inline int SelectJitterPhaseCount(int mode, float ratio)
{
    return mode == 1 || (mode == 2 && ratio == 1.0f)
        ? FsrRecommendedJitterPhases(ratio) : 1024;
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
                                       int phaseCount = 1024)
{
    if (originalW <= 0 || originalH <= 0 || renderW <= 0 || renderH <= 0) return {oldX, oldY};
    if (phaseCount<1 || phaseCount>1024) phaseCount=1024;
    if (phaseCount==1024 && std::isfinite(oldX) && std::isfinite(oldY) && (std::fabs(oldX) > 1e-8f || std::fabs(oldY) > 1e-8f))
        return { oldX * originalW / renderW, oldY * originalH / renderH };
    auto phase = (int64_t{frame} + phaseOffset) % phaseCount;
    if (phase < 0) phase += phaseCount;
    // A camera with TAA disabled may leave its settings zero-initialized.
    const float scale = std::isfinite(jitterScale) && jitterScale > 0 && jitterScale <= 2 ? jitterScale : 1;
    return { (TaaHalton(static_cast<uint32_t>(phase + 1), 2) - 0.5f) * scale * 2 / renderW,
             (TaaHalton(static_cast<uint32_t>(phase + 1), 3) - 0.5f) * scale * 2 / renderH };
}
