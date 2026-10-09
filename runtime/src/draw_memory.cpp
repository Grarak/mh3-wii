// Model draw guest-memory view (draw_memory.h).
#include "memory.h"
#include "draw_memory.h"
#include <cstdio>
#include <stdexcept>

thread_local const mh3_res_memory* DrawMemory::view = nullptr;

uint8_t* DrawMemory::ViewPointer(uint32_t address, size_t length, bool write) {
    char message[96];
    size_t offset = 0;
    if (const auto* region = mh3_res_memory_region(view, address, length, &offset)) {
        if (!write || region->writable)
            return const_cast<uint8_t*>(region->span.data) + offset;
        std::snprintf(message, sizeof message, "Draw view write to read-only region at %08x+%zu", address, length);
        throw std::runtime_error(message);
    }
    mh3_res_memory_view mapped{};
    if (!write && mh3_res_runtime_map(view, address, length, &mapped) == MH3_RES_OK)
        return const_cast<uint8_t*>(mapped.span.data + mapped.offset);
    std::snprintf(message, sizeof message, "Draw view %s outside its regions at %08x+%zu", write ? "write" : "read", address, length);
    throw std::runtime_error(message);
}
