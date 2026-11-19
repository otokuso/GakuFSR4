#pragma once
#include <atomic>

class FgDropNotice
{
    std::atomic<const void*> target_{nullptr};
    std::atomic<const void*> alias_{nullptr};
    std::atomic<bool> missed_{false};

public:
    // Non-owning DXGI interface aliases. Holding a reference would block
    // replacement of a flip-model swap chain on the same window.
    void SetTarget(const void* chain, const void* alternate = nullptr) {
        alias_.store(alternate, std::memory_order_release);
        target_.store(chain, std::memory_order_release);
    }
    bool IsTarget(const void* chain) const {
        return chain && (chain == target_.load(std::memory_order_acquire) ||
                         chain == alias_.load(std::memory_order_acquire));
    }
    void Mark(const void* chain) {
        if (IsTarget(chain)) missed_.store(true, std::memory_order_release);
    }
    bool Consume() { return missed_.exchange(false, std::memory_order_acq_rel); }
};