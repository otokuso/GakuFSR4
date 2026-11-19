#pragma once
#include <cstdint>

// Unity command buffers can outlive the C++ call that recorded their blits.
// Reclaim an old texture set only after the last referring render event ran.
inline bool FinishedUsingResources(uint64_t lastSubmittedEvent, uint64_t completedEvents)
{
    return completedEvents >= lastSubmittedEvent;
}
