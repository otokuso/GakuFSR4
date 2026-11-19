#pragma once
#include <algorithm>
#include <cstdint>

struct FgImageRect
{
    uint32_t x, y, width, height;
};

inline bool FgMatchingAspectRatio(uint32_t w, uint32_t h, uint32_t sceneW, uint32_t sceneH)
{
    if (!w || !h || !sceneW || !sceneH) return false;
    const double ratio = double(w) * sceneH / (double(h) * sceneW);
    return ratio >= 0.99 && ratio <= 1.01;
}

inline FgImageRect FgAspectFit(uint32_t w, uint32_t h, uint32_t sceneW, uint32_t sceneH)
{
    if (!w || !h || !sceneW || !sceneH) return {};
    if (FgMatchingAspectRatio(w, h, sceneW, sceneH)) return {0, 0, w, h};
    const double scale = (std::min)(double(w) / sceneW, double(h) / sceneH);
    const auto width = (std::min)(w, (std::max)(1u, static_cast<uint32_t>(scale * sceneW + 0.5)));
    const auto height = (std::min)(h, (std::max)(1u, static_cast<uint32_t>(scale * sceneH + 0.5)));
    return {(w - width) / 2, (h - height) / 2, width, height};
}
