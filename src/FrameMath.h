#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace frame_math {
constexpr bool SceneCamera(bool game, bool base, bool resolves, bool xr, bool orthographic) {
    return game && base && resolves && !xr && !orthographic;
}

constexpr bool SupportedTarget(int dimension, int samples, bool dynamicScale) {
    return dimension == 2 && samples == 1 && !dynamicScale;
}

constexpr bool UpscaleSource(int width, int height, int renderW, int renderH, int outputW, int outputH) {
    return width==renderW && height==renderH && width>0 && height>0 && width<outputW && height<outputH;
}

constexpr bool RetryCamera(int camera, int blockedCamera, int frame, int retryFrame) {
    return camera != blockedCamera || frame >= retryFrame;
}

// The existing RTHandle pair has display dimensions only in NativeAA mode.
// Earlier upscaling at ratio > 1 requires rebuilding URP's later post passes.
constexpr bool EarlyNativeAA(bool offscreen, float ratio) {
    return !offscreen && ratio==1.0f;
}

// Never hide a scaled input/output behind a 1:1 NativeAA preset. The TAA
// RTHandles are expected to be real, fixed-size render textures in this path.
constexpr bool NativeSurfaceDimensions(int srcW,int srcH,int dstW,int dstH,int expectedW,int expectedH) {
    return expectedW>0 && expectedH>0 && srcW==expectedW && srcH==expectedH &&
        dstW==expectedW && dstH==expectedH;
}

constexpr bool FsrInputIsHdr(bool cameraHdr, bool offscreen, bool beforePost) {
    return cameraHdr && (offscreen || beforePost);
}

inline int RenderSize(int display, float ratio) {
    if (display < 1 || display > 16384 || !std::isfinite(ratio) || ratio < 1 || ratio > 3)
        throw std::runtime_error("Invalid render dimensions or ratio");
    return (std::max)(1, static_cast<int>(display / ratio));
}
inline float Jitter(float matrixOffset, int renderSize, int sign = -1) {
    return sign * matrixOffset * renderSize * 0.5f;
}
inline bool RotationJump(const float* forward, const float* previousForward) {
    float dot = 0, a = 0, b = 0;
    for (int i = 0; i < 3; ++i) {
        dot += forward[i] * previousForward[i];
        a += forward[i] * forward[i];
        b += previousForward[i] * previousForward[i];
    }
    if (!(a > 0.000001f) || !(b > 0.000001f)) return true;
    // More than 45 degrees between two consecutive frames is a likely camera cut.
    return !(dot / std::sqrt(a * b) >= 0.70710678f);
}
inline float MotionVectorYScale(int renderHeight, int sign) {
    return static_cast<float>(renderHeight) * sign;
}
inline bool Reset(int camera, int oldCamera, int frame, int oldFrame,
                  float fov, float oldFov, const float* pos, const float* oldPos) {
    if (!std::isfinite(fov) || !std::isfinite(oldFov)) return true;
    float distance = 0;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(pos[i]) || !std::isfinite(oldPos[i])) return true;
        distance += (pos[i]-oldPos[i])*(pos[i]-oldPos[i]);
    }
    // The frame counter is a signed int in Unity; oldFrame + 1 can overflow.
    return camera != oldCamera || int64_t{frame} != int64_t{oldFrame} + 1 ||
        distance > 25.0f || std::abs(fov-oldFov) > 5;
}
}
