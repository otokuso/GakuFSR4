#pragma once
#include <wchar.h>

inline constexpr struct FsrPreset {
    const wchar_t* key;
    const char* label;
    float ratio;
} FsrPresets[]{
    {L"NativeAA","Native AA",1.0f},
    {L"Quality","Quality",1.5f},
    {L"Balanced","Balanced",1.7f},
    {L"Performance","Performance",2.0f},
    {L"UltraPerformance","Ultra Performance",3.0f},
};

inline float PresetRatio(const wchar_t* name) {
    for (const auto& preset : FsrPresets) {
        if (!_wcsicmp(name,preset.key)) return preset.ratio;
    }
    return 0.0f;
}
