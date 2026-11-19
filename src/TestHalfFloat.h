#pragma once
#include <cstdint>
#include <cstring>

// Small test-harness helper for generating R16G16_FLOAT motion vectors.
inline uint16_t TestFloatToHalf(float input)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &input, sizeof bits);
    const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
    int exponent = static_cast<int>((bits >> 23) & 255u) - 127 + 15;
    if (exponent <= 0) return sign; // test velocities are above half's normal threshold
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
    uint32_t fraction = ((bits & 0x7fffffu) + 0x1000u) >> 13;
    if (fraction == 0x400u) { fraction = 0; ++exponent; }
    return static_cast<uint16_t>(sign | (exponent << 10) | fraction);
}
