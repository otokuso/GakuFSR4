#pragma once

#include <cstdint>

// An FG-only circuit breaker. The upscaler keeps running while FG cools down.
struct FgRetry
{
    static constexpr uint32_t MaxFailures = 6;
    uint32_t failures = 0;
    uint64_t retryAtMs = 0;
    bool permanent = false;
    uint32_t sceneW = 0, sceneH = 0, renderW = 0, renderH = 0;
    uint32_t backbufferW = 0, backbufferH = 0;

    void Recovered()
    {
        failures = 0;
        retryAtMs = 0;
    }

    void NewDevice()
    {
        permanent = false;
        sceneW = sceneH = renderW = renderH = 0;
        backbufferW = backbufferH = 0;
        Recovered();
    }

    void ObserveScene(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh)
    {
        if (w == sceneW && h == sceneH && rw == renderW && rh == renderH) return;
        sceneW = w; sceneH = h; renderW = rw; renderH = rh;
        if (!permanent) Recovered();
    }

    void ObserveBackbuffer(uint32_t w, uint32_t h)
    {
        if (!w || !h || (w == backbufferW && h == backbufferH)) return;
        backbufferW = w; backbufferH = h;
        if (!permanent) Recovered();
    }

    bool Ready(uint64_t nowMs) const
    {
        return !permanent && failures < MaxFailures && nowMs >= retryAtMs;
    }

    // Returns retry delay, or zero if retrying is permanently/temporarily exhausted.
    uint32_t Failed(uint64_t nowMs, bool unsupported)
    {
        if (unsupported) { permanent = true; return 0; }
        if (failures < MaxFailures) ++failures;
        const uint32_t delayMs = 500u << (failures - 1);
        retryAtMs = nowMs + delayMs;
        return failures == MaxFailures ? 0 : delayMs;
    }
};
