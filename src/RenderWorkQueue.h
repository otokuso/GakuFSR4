#pragma once

#include "ResourceLifetime.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

// Unity provides no destruction callback when a CommandBuffer drops its plugin events.
// Keep the texture use until the completion event, but make unconsumed packets cancelable.
template<class Packet, std::size_t Capacity>
class RenderWorkQueue {
    struct Work {
        std::uintptr_t id = 0;
        ResourceLifetime* lifetime = nullptr;
        std::unique_ptr<Packet> packet;
    };

    std::mutex mutex;
    std::array<Work, Capacity> work;
    std::uintptr_t next = 0;
    bool accepting = true;

    Work* Find(std::uintptr_t id) {
        for(auto& item : work) {
            if(item.lifetime && item.id == id) return &item;
        }
        return nullptr;
    }

public:
    std::uintptr_t Begin(ResourceLifetime& lifetime) {
        const std::lock_guard lock(mutex);
        if(!accepting) return 0;
        for(auto& item : work) {
            if(item.lifetime) continue;
            if(++next == 0) ++next;
            lifetime.Begin();
            item.id = next;
            item.lifetime = &lifetime;
            return item.id;
        }
        return 0;
    }

    bool Attach(std::uintptr_t id, std::unique_ptr<Packet> packet) {
        const std::lock_guard lock(mutex);
        auto* item = Find(id);
        if(!accepting || !item || item->packet) return false;
        item->packet = std::move(packet);
        return true;
    }

    std::unique_ptr<Packet> Take(std::uintptr_t id) {
        const std::lock_guard lock(mutex);
        auto* item = Find(id);
        return item ? std::move(item->packet) : nullptr;
    }

    void Complete(std::uintptr_t id) {
        std::unique_ptr<Packet> discarded;
        const std::lock_guard lock(mutex);
        if(auto* item = Find(id)) {
            discarded = std::move(item->packet);
            auto* lifetime = item->lifetime;
            item->lifetime = nullptr;
            lifetime->Complete();
        }
    }

    void CancelPackets() {
        std::array<std::unique_ptr<Packet>, Capacity> discarded;
        const std::lock_guard lock(mutex);
        accepting = false;
        for(std::size_t i = 0; i < work.size(); ++i) discarded[i] = std::move(work[i].packet);
    }

    std::size_t Pending() {
        const std::lock_guard lock(mutex);
        std::size_t pending = 0;
        for(const auto& item : work) {
            if(item.lifetime) ++pending;
        }
        return pending;
    }
};
