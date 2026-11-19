#pragma once
#include <atomic>

// Communicates a missed Present without taking the render lock. The next
// successful lock acquisition must invalidate the previous FG input history.
struct FgDropNotice
{
    std::atomic<bool> missed{false};
    void Mark() { missed.store(true, std::memory_order_release); }
    bool Consume() { return missed.exchange(false, std::memory_order_acq_rel); }
};
