#include "FrameMath.h"
#include "FgViewportMath.h"
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
    auto portraitOnLandscape = FgAspectFit(1920,1080,1080,1920);
    assert(FgViewportWithinBackbuffer({400,100,1080,608},1920,1080,1920,1080));
    assert(!FgViewportWithinBackbuffer({1900,100,1080,608},1920,1080,1920,1080));
    assert(!FgViewportWithinBackbuffer({400,100,1080,608},1920,1080,1080,1920));
    assert(portraitOnLandscape.x==656 && portraitOnLandscape.y==0);
    assert(portraitOnLandscape.width==608 && portraitOnLandscape.height==1080);
    auto landscapeOnPortrait = FgAspectFit(1080,1920,1920,1080);
    assert(landscapeOnPortrait.x==0 && landscapeOnPortrait.y==656);
    assert(landscapeOnPortrait.width==1080 && landscapeOnPortrait.height==608);
    auto letterbox = FgAspectFit(1920,1080,3840,1634);
    assert(letterbox.x==0 && letterbox.y==131 && letterbox.width==1920 && letterbox.height==817);
    auto same = FgAspectFit(1920,1080,1280,720);
    assert(same.x==0 && same.y==0 && same.width==1920 && same.height==1080);
    assert(FgAspectFit(1920,1080,0,1080).width==0);
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
