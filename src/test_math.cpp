#include "FrameMath.h"
#include "TaaJitter.h"
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
    auto generated=RepairTaaJitter(0,0,1920,1080,1280,720,1,0,1.0f);
    assert(std::isfinite(generated.x) && std::isfinite(generated.y));
    assert(std::fabs(generated.x)+std::fabs(generated.y)>0);
    auto generatedNext=RepairTaaJitter(0,0,1920,1080,1280,720,2,0,1.0f);
    assert(generated.x!=generatedNext.x || generated.y!=generatedNext.y);
    auto rescaled=RepairTaaJitter(0.25f/1920,0.5f/1080,1920,1080,1280,720,1,0,1.0f);
    assert(std::fabs(rescaled.x-0.25f/1280)<0.000001f);
    assert(std::fabs(rescaled.y-0.5f/720)<0.000001f);
    assert(std::fabs(TaaHalton(1,2)-0.5f)<0.000001f);
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
