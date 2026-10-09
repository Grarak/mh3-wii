// Model draw guest-memory view (draw_memory.h).
#include "memory.h"
#include "draw_memory.h"
#include <cstdio>
#include <stdexcept>

static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> DrawMemory::detail::viewsEverEnabled{false};
namespace DrawMemory::detail {
thread_local const mh3_res_memory* view = nullptr;
const mh3_res_memory* EnabledView() { return view; }
}
thread_local bool DrawMemory::liveWrites = false;

void DrawMemory::InstallView(const mh3_res_memory* next) {
    if (next) {
        detail::viewsEverEnabled.store(true, std::memory_order_relaxed);
        detail::view = next;
    } else if (detail::viewsEverEnabled.load(std::memory_order_relaxed)) {
        detail::view = nullptr;
    }
}

uint8_t* DrawMemory::ViewPointer(uint32_t address, size_t length, bool write) {
    const mh3_res_memory* view = detail::view;
    char message[96];
    size_t offset = 0;
    if (const auto* region = mh3_res_memory_region(view, address, length, &offset)) {
        if (!write || region->writable)
            return const_cast<uint8_t*>(region->span.data) + offset;
        std::snprintf(message, sizeof message, "Draw view write to read-only region at %08x+%zu", address, length);
        throw std::runtime_error(message);
    }
    mh3_res_memory_view mapped{};
    if (write && liveWrites)
        return Memory::GetPointer(address, length);
    if (!write && mh3_res_runtime_map(view, address, length, &mapped) == MH3_RES_OK)
        return const_cast<uint8_t*>(mapped.span.data + mapped.offset);
    std::snprintf(message, sizeof message, "Draw view %s outside its regions at %08x+%zu", write ? "write" : "read", address, length);
    throw std::runtime_error(message);
}
