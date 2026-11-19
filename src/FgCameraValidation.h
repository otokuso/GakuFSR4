#pragma once

#include <cmath>
#include <initializer_list>

// Scene transitions can temporarily expose invalid camera values. FSR upscale
// does not require camera axes, so reject only FG preparation for those frames.
namespace fg_camera {
inline bool Valid(float nearPlane, float farPlane, float verticalFov,
                  const float* position, const float* up,
                  const float* right, const float* forward)
{
    if (!std::isfinite(nearPlane) || !std::isfinite(farPlane) ||
        !(nearPlane > 0) || !(farPlane > nearPlane) ||
        !std::isfinite(verticalFov) || !(verticalFov > 0) || !(verticalFov < 3.14159265f))
        return false;
    for (const float* axis : {position, up, right, forward}) {
        if (!axis) return false;
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(axis[i])) return false;
    }
    return true;
}
}
