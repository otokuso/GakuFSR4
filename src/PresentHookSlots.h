#pragma once
#include <map>

// Store original functions by their actual COM vtable slot, not by a single
// global Present function that a subsequent SwapChain hook could overwrite.
class PresentHookSlots
{
    std::map<void**, void*> originals_;
public:
    bool Remember(void** slot, void* original)
    {
        return originals_.emplace(slot, original).second;
    }
    void* Find(void** slot) const
    {
        auto it = originals_.find(slot);
        return it == originals_.end() ? nullptr : it->second;
    }
};
