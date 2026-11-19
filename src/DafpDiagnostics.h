#pragma once

#include <cstdint>

// Snapshot of conditions observable to GakuFSR4. The AMD FSR API does not expose
// whether the display driver actually selected driver-assisted frame pacing.
struct DafpDiagnostics {
    void* window = nullptr; // HWND, kept opaque for the non-Windows unit test.
    uint32_t swapchainMajor = 0; // 0 = provider version could not be determined.
    uint32_t syncInterval = 0;
    bool nativeSwapchain = false;
    bool presentRecent = false;
    bool generatedRecent = false;
};

enum class DafpEligibility {
    Inactive,
    Blocked,
    Unknown,
    PossibleUnverified,
};

// fullscreenCoverage: 1 = covered, 0 = not covered, -1 = unknown. Driver
// version, VRR support/range and the driver's chosen pacing mode are unknown.
inline DafpEligibility EvaluateDafpEligibility(const DafpDiagnostics& d, int fullscreenCoverage) {
    if (!d.nativeSwapchain || !d.presentRecent || !d.generatedRecent)
        return DafpEligibility::Inactive;
    if (d.syncInterval != 0 || fullscreenCoverage == 0 || (d.swapchainMajor && d.swapchainMajor < 4))
        return DafpEligibility::Blocked;
    if (!d.swapchainMajor || fullscreenCoverage < 0) return DafpEligibility::Unknown;
    return DafpEligibility::PossibleUnverified;
}

// Extract a version such as "4.0.0" or "AMD SwapChain 4.0.0". A standalone
// integer is not a version; avoid reporting a misleading SDK generation.
inline uint32_t DafpSwapchainMajor(const char* version) {
    if (!version) return 0;
    for (const char* p = version; *p; ++p) {
        if (*p < '0' || *p > '9' || (p != version && p[-1] >= '0' && p[-1] <= '9')) continue;
        uint32_t major = 0;
        const char* digit = p;
        while (*digit >= '0' && *digit <= '9') {
            if (major > 100) break;
            major = major * 10 + (*digit++ - '0');
        }
        if (major <= 100 && *digit == '.' && digit[1] >= '0' && digit[1] <= '9') {
            while (*++digit >= '0' && *digit <= '9') {}
            if (*digit == '.' && digit[1] >= '0' && digit[1] <= '9') return major;
        }
    }
    return 0;
}
