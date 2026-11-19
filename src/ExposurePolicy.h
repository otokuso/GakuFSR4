#pragma once
#include <cmath>

// A fabricated constant of 1.0 is not a measurement of the game's exposure.
// When no valid value is captured, keep the SDK auto-exposure path enabled.
inline bool HasExternalExposure(bool providerRequiresExposure, float exposure)
{
    return providerRequiresExposure && std::isfinite(exposure) && exposure > 0;
}
