// Inline host pointer of a guest range for native producers (a cached MEM1 or
// MEM2 address): straight through the flat window, unchecked like Memory::Read
// (memory_access.h). With MH3_AUDIT_HOST_ACCESS any other range is reported
// and goes to Memory::GetPointer.
#pragma once
#include "guest_flat_memory.h"
#include "memory.h"
#include <cstddef>
#include <cstdint>

inline uint8_t* GuestRange(uint32_t address, size_t length) {
#ifdef MH3_AUDIT_HOST_ACCESS
    if (!(address - 0x80000000u <= (24u << 20) - length || address - 0x90000000u <= (64u << 20) - length)) {
        MemoryHost::AuditHostAccess(address, length);
        return Memory::GetPointer(address, length);
    }
#else
    (void)length;
#endif
    return GuestFlat::HostCached(address);
}
