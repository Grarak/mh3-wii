// Inline host pointer of a guest range for native producers. Mapped RAM
// resolves without a call; anything else goes to Memory::GetPointer, which
// reports the unmapped access as before.
#pragma once
#include "guest_flat_memory.h"
#include "memory.h"
#include <cstddef>
#include <cstdint>

inline uint8_t* GuestRange(uint32_t address, size_t length) {
    if (uint8_t* pointer = GuestFlat::HostPointer(address, length)) [[likely]]
        return pointer;
    return Memory::GetPointer(address, length);
}
