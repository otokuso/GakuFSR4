#pragma once
#include <wchar.h>

inline float PresetRatio(const wchar_t* name) {
    if (!_wcsicmp(name, L"NativeAA")) return 1.0f;
    if (!_wcsicmp(name, L"Quality")) return 1.5f;
    if (!_wcsicmp(name, L"Balanced")) return 1.7f;
    if (!_wcsicmp(name, L"Performance")) return 2.0f;
    if (!_wcsicmp(name, L"UltraPerformance")) return 3.0f;
    return 0.0f;
}
