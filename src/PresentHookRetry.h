#pragma once

#include <cstdint>

// A failed dummy DXGI swapchain probe must not disable later Present hooking.
// Throttle retries so persistent driver/window failures do not recreate a
// window and swapchain on every rendered frame.
class PresentHookRetry {
    bool attempted_ = false;
    uint64_t lastAttemptMs_ = 0;
public:
    static constexpr uint64_t RetryIntervalMs = 5000;

    bool Allow(uint64_t nowMs) {
        if (attempted_ && nowMs >= lastAttemptMs_ &&
            nowMs - lastAttemptMs_ < RetryIntervalMs) return false;
        attempted_ = true;
        lastAttemptMs_ = nowMs;
        return true;
    }
};
