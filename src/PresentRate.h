#pragma once

#include <cstdint>

struct PresentCounts {
    uint64_t real = 0;
    uint64_t generated = 0;
};

bool IsGameFpsWindow(void* window);
void CountPresentFrames(uint64_t real, uint64_t generated);
PresentCounts ReadPresentCounts();

struct PresentRate {
    uint64_t sampledAt = 0;
    PresentCounts previous{};
    double realFps = 0;
    double totalFps = 0;
    bool ready = false;

    void Sample(uint64_t now, PresentCounts current) {
        const auto elapsed = now - sampledAt;
        if(sampledAt && elapsed < 1000) return;
        ready = sampledAt && elapsed <= 3000 && (current.real || current.generated)
            && current.real >= previous.real && current.generated >= previous.generated;
        if(ready) {
            const auto real = current.real - previous.real;
            const auto generated = current.generated - previous.generated;
            realFps = 1000.0 * real / elapsed;
            totalFps = 1000.0 * (real + generated) / elapsed;
        }
        previous = current;
        sampledAt = now;
    }
};
