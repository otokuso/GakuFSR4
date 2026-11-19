#include "FgDistortionMath.h"
#include <cassert>
#include <cmath>

int main() {
    using fg_distortion::RadialOffset;
    const auto atCenter=RadialOffset(.5f,.5f,.5f,.5f,.3f,-.1f);
    assert(atCenter.x==0 && atCenter.y==0);
    const auto noDistortion=RadialOffset(.9f,.2f,.5f,.5f,0,0);
    assert(noDistortion.x==0 && noDistortion.y==0);
    const auto right=RadialOffset(.75f,.5f,.5f,.5f,1,0);
    const auto left=RadialOffset(.25f,.5f,.5f,.5f,1,0);
    assert(std::abs(right.x-.015625f)<1e-7f);
    assert(std::abs(left.x+right.x)<1e-7f);
    assert(left.y==0 && right.y==0);
}
