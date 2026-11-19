#pragma once

#include <cmath>

namespace fg_distortion {
struct Offset { float x, y; };

// Offset = distorted UV - original UV. This is a manually calibrated radial
// approximation, not a reconstruction of Unity Lens Distortion parameters.
inline Offset RadialOffset(float u, float v, float centerX, float centerY, float k1, float k2) {
    const float x=u-centerX, y=v-centerY;
    const float r2=x*x+y*y;
    const float delta=k1*r2+k2*r2*r2;
    return {x*delta,y*delta};
}
}
