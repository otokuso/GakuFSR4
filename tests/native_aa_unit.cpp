#include "FrameMath.h"
#include "TaaJitter.h"
#include <cassert>
#include <cmath>

int main() {
    assert(frame_math::EarlyNativeAA(false,1.0f));
    assert(!frame_math::EarlyNativeAA(true,1.0f));
    assert(!frame_math::EarlyNativeAA(false,1.5f));

    assert(frame_math::NativeSurfaceDimensions(3840,2160,3840,2160,3840,2160));
    assert(!frame_math::NativeSurfaceDimensions(3840,2160,3840,2159,3840,2160));
    assert(!frame_math::NativeSurfaceDimensions(2560,1440,3840,2160,3840,2160));

    assert(SelectJitterPhaseCount(0,1.0f)==1024);
    assert(SelectJitterPhaseCount(1,1.0f)==8);
    assert(SelectJitterPhaseCount(2,1.0f)==8);
    assert(SelectJitterPhaseCount(2,1.5f)==1024);
    assert(SelectJitterPhaseCount(1,1.5f)==18);
    const auto a=RepairTaaJitter(0,0,3840,2160,3840,2160,5,0,1,8);
    const auto b=RepairTaaJitter(0,0,3840,2160,3840,2160,13,0,1,8);
    assert(a.x==b.x && a.y==b.y);
    assert(std::isfinite(a.x) && std::isfinite(a.y));
    const auto legacy=RepairTaaJitter(0.5f/3840,0.25f/2160,3840,2160,3840,2160,5,0,1,1024);
    assert(std::fabs(legacy.x-0.5f/3840)<0.000001f);
    assert(std::fabs(legacy.y-0.25f/2160)<0.000001f);
    assert(frame_math::FsrInputIsHdr(true,false,true));
    assert(!frame_math::FsrInputIsHdr(true,false,false));
}
