#pragma once
#include <stdexcept>
#include <utility>

using GCHandle = void*; // IL2CPP in Unity 6 uses pointer-sized handles.

template<auto& api>
class ManagedRoot {
    GCHandle handle = nullptr;
public:
    ManagedRoot() = default;
    explicit ManagedRoot(void* object)
        : handle(object ? api.gchandle_new(object, false) : nullptr) {
        if (object && !handle) throw std::runtime_error("GC handle allocation failed");
    }
    ManagedRoot(const ManagedRoot&) = delete;
    ManagedRoot& operator=(const ManagedRoot&) = delete;
    ManagedRoot(ManagedRoot&& other) noexcept
        : handle(std::exchange(other.handle, nullptr)) {}
    ManagedRoot& operator=(ManagedRoot&& other) noexcept {
        if (this != &other) {
            Clear();
            handle = std::exchange(other.handle, nullptr);
        }
        return *this;
    }
    void* Get() const { return handle ? api.gchandle_get_target(handle) : nullptr; }
    void Clear() { if (handle) api.gchandle_free(std::exchange(handle, nullptr)); }
    ~ManagedRoot() { Clear(); }
};
