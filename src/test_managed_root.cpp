#include "ManagedRoot.h"
#include <cstdint>

int first, second, released;
bool truncated;
const auto firstHandle = reinterpret_cast<GCHandle>(uintptr_t{0x123400000001});
const auto secondHandle = reinterpret_cast<GCHandle>(uintptr_t{0x567800000001});

struct FakeApi {
    GCHandle gchandle_new(void* object, bool) {
        return object == &first ? firstHandle : secondHandle;
    }
    void* gchandle_get_target(GCHandle handle) {
        if (handle == firstHandle) return &first;
        if (handle == secondHandle) return &second;
        truncated = true;
        return nullptr;
    }
    void gchandle_free(GCHandle handle) {
        if (handle == firstHandle) released |= 1;
        else if (handle == secondHandle) released |= 2;
        else truncated = true;
    }
} fake;

int main() {
    {
        ManagedRoot<fake> root(&first);
        if (root.Get() != &first) return 1;
        root = ManagedRoot<fake>(&second); // Same replacement as the next camera frame.
        if (released != 1 || root.Get() != &second) return 2;
    }
    return !truncated && released == 3 ? 0 : 3;
}
