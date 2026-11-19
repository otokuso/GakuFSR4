#pragma once
#include <atomic>

class ResourceLifetime {
    std::atomic<unsigned> pending{0};
public:
    void Begin() { pending.fetch_add(1,std::memory_order_relaxed); }
    void Complete() { pending.fetch_sub(1,std::memory_order_release); }
    bool InUse() const { return pending.load(std::memory_order_acquire)!=0; }
};
