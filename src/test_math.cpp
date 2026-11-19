#include "FrameMath.h"
#include "Preset.h"
#include <cassert>
static_assert(frame_math::SceneCamera(true,true,true,false,false));
static_assert(!frame_math::SceneCamera(true,true,false,false,false));
static_assert(!frame_math::SceneCamera(true,false,true,false,false));
static_assert(!frame_math::SceneCamera(true,true,true,true,false));
static_assert(!frame_math::SceneCamera(true,true,true,false,true));
static_assert(frame_math::SupportedTarget(2,1,false));
static_assert(!frame_math::SupportedTarget(4,1,false));
static_assert(!frame_math::SupportedTarget(2,4,false));
static_assert(!frame_math::SupportedTarget(2,1,true));
static_assert(frame_math::UpscaleSource(1440,2560,1440,2560,2160,3840));
static_assert(!frame_math::UpscaleSource(2160,3840,1440,2560,2160,3840));
static_assert(!frame_math::UpscaleSource(1441,2560,1440,2560,2160,3840));
static_assert(!frame_math::RetryCamera(4,4,29,30));
static_assert(frame_math::RetryCamera(4,4,30,30));
static_assert(frame_math::RetryCamera(5,4,29,30));
int main() {
    float prevForward[3]{0,0,1};
    float smoothForward[3]{0.173648f,0,0.984808f}; // 10-degree pan
    float jumpForward[3]{0.866025f,0,0.5f}; // 60-degree cut
    float reverseForward[3]{0,0,-1};
    float zeroForward[3]{};
    assert(!frame_math::RotationJump(prevForward,prevForward));
    assert(!frame_math::RotationJump(smoothForward,prevForward));
    assert(frame_math::RotationJump(jumpForward,prevForward));
    assert(frame_math::RotationJump(reverseForward,prevForward));
    assert(frame_math::RotationJump(prevForward,zeroForward));
    assert(PresetRatio(L"NativeAA")==1.0f);
    assert(PresetRatio(L"quality")==1.5f);
    assert(PresetRatio(L"Balanced")==1.7f);
    assert(PresetRatio(L"Performance")==2.0f);
    assert(PresetRatio(L"UltraPerformance")==3.0f);
    assert(PresetRatio(L"unknown")==0.0f);
    assert(frame_math::RenderSize(1920,1.5f)==1280);
    assert(frame_math::RenderSize(1080,1.5f)==720);
    assert(frame_math::RenderSize(2160,1.5f)==1440);
    assert(frame_math::RenderSize(3840,1.5f)==2560);
    assert(frame_math::RenderSize(1,3)==1);
    bool rejected=false;
    try { frame_math::RenderSize(0,1.5f); } catch (...) { rejected=true; }
    assert(rejected);
    assert(std::abs(frame_math::Jitter(0.5f/640,1280)+0.5f)<0.00001f);
    float a[3]{}, b[3]{6,0,0};
    assert(!frame_math::Reset(1,1,2,1,45,45,a,a));
    assert(frame_math::Reset(2,1,2,1,45,45,a,a));
    assert(frame_math::Reset(1,1,4,1,45,45,a,a));
    assert(frame_math::Reset(1,1,2,1,45,45,a,b));
    assert(frame_math::Reset(1,1,2,1,51,45,a,a));
}
