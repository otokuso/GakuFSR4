#pragma once

#include <cstdint>

// Shared between the bridge, the GakuLoader panel and the exported Stats ABI.
// Keep the 680-byte layout synchronized with the external test harness.
struct Stats
{
    uint64_t frames;
    uint64_t providerId;
    int32_t lastResult;
    int32_t failed;
    char provider[64];
    char adapter[128];
    uint64_t presented;
    float cpuMs;
    float gpuMs;
    uint64_t fsrMemory;
    uint64_t logDropped;
    uint32_t bridgeStarts;
    uint32_t contextStarts;
    int32_t removedReason;
    uint32_t failures;
    float gpuPeriodMs, gpuPeriodMin, gpuPeriodMax;
    float gpuBusyMs, gpuBusyMin, gpuBusyMax;
    uint64_t gpuFrames;
    uint64_t gpuDisjoint;
    uint64_t gpuDropped;
    float markMs[48];
    float flushMs;
    uint32_t exposureWanted;
    uint64_t directFrames;
    char fgProvider[64];
    uint64_t fgProviderId, fgPrepared, fgGenerated, fgReal, fgSkipped, fgMemory;
    int32_t fgLastResult;
    float fgGpuMs, fgPaceMs, fgIntervalMs;
    float fgHookMs;
    float fgRefreshHz;
    uint64_t fgNotNeeded;
    uint64_t presentsHooked;
    uint64_t fgHudless;
};

static_assert(sizeof(Stats) == 680, "Unexpected native Stats ABI layout");
