#pragma once

// Flat guest address space. Guest accesses use Host(addr) plus a byte swap,
// without a page table lookup or per-access check. Desktop hosts reserve the
// whole window PROT_NONE and map RAM from one shared-memory object per alias.
// Vita folds aliases into fixed kubridge views of the same physical backing:
//
//   MEM1   24 MiB  0x80000000 (cached)  0xC0000000 (uncached)  0x00000000 (physical, 64-bit only)
//   MEM2   64 MiB  0x90000000           0xD0000000             0x10000000 (physical, 64-bit only)
//   locked cache 16 KiB at 0xE0000000
//
// Hardware (MMIO at 0xCC000000..0xCDFFFFFF, the EFB at 0xC8000000) and every
// other address stay PROT_NONE. A translated function that touches hardware
// is listed in runtime/src/hardware_access.inc and uses MemoryHw instead; any
// other access to an unmapped address faults, and ReportGuestFault names the
// guest address and the translated function doing it.
#include <cstddef>
#include <cstdint>

namespace GuestFlat {

#if defined(__vita__)
// DSVita places its guest MMU windows at 0xB0000000 and 0xC0000000.
// Fold Wii cached/uncached/physical aliases before adding the Vita base:
// MEM1 -> B0000000, MEM2 -> C0000000, locked cache -> B4000000.
// D0000000 is outside the usable reservation partition on firmware 3.65.
// These virtual views are reserved with kubridge, without physical gap pages.
inline constexpr uintptr_t kGuestBase = 0xB000'0000u;
#elif defined(__x86_64__)
inline constexpr uintptr_t kGuestBase = 0x0000'1000'0000'0000ull;
#elif defined(__aarch64__)
// Reachable on every AArch64 VA width in use (39-bit and up), far below where
// the PIE image, heap, libraries and stack land.
inline constexpr uintptr_t kGuestBase = 0x0000'0010'0000'0000ull;
#elif defined(__arm__)
// A 32-bit process can't reserve 4 GiB, so the window wraps: the host address
// is (kGuestBase + addr) mod 2^32. Guest 0x80000000..0xE0004000 lands at host
// 0x10000000..0x70004000, above the executable and below the mmap area.
// Physical guest addresses (0x0.../0x1...) fall outside the window; only host
// code uses them, through Memory::GetPointer.
#define MH3_GUEST_WINDOW_WRAPS 1
inline constexpr uintptr_t kGuestBase = 0x9000'0000u;
#else
#error "guest_flat_memory.h has no guest base for this architecture"
#endif

// The host address of a guest address.
inline uint8_t* Host(uint32_t guestAddress) noexcept {
#if defined(__vita__)
    const uint32_t offset = (guestAddress & 0x1fffffffu) | ((guestAddress >> 3) & 0x04000000u);
    return reinterpret_cast<uint8_t*>(kGuestBase + offset);
#else
    return reinterpret_cast<uint8_t*>(kGuestBase + guestAddress);
#endif
}

// Reserves the window and maps MEM1, MEM2 and the locked cache into it from a
// fresh shared-memory object (zero filled), then installs the fault reporter.
// Throws std::runtime_error when the window is already occupied. Calling it
// again remaps fresh zeroed memory.
void Initialize();

// True for the guest addresses of the MMIO blocks and the EFB: the ones only
// MemoryHw and the host API may touch.
constexpr bool IsHardwareAddress(uint32_t address) noexcept {
    const uint32_t top = address >> 24;
    return (top & 0xFE) == 0xCC || top == 0xC8;
}

// Set once Initialize has mapped guest memory.
inline bool g_mapped = false;

// Host pointer for a mapped guest address, physical aliases included; nullptr
// when [address, address + length) is not wholly mapped. Host code only.
inline uint8_t* HostPointer(uint32_t address, size_t length) noexcept {
    // Physical addresses read through the cached alias.
    if (address >> 30 == 0)
        address |= 0x80000000u;
    uint32_t size;
    switch (address >> 28) {
    case 0x8:
    case 0xC:
        size = 24u << 20;
        break; // MEM1
    case 0x9:
    case 0xD:
        size = 64u << 20;
        break; // MEM2
    case 0xE:
        size = 0x4000;
        break; // locked cache
    default:
        return nullptr;
    }
    const uint32_t offset = address & 0x0FFFFFFFu;
    if (length == 0 || offset >= size || length > size - offset || !g_mapped)
        return nullptr;
    return Host(address);
}

// The guest address range [address, address + length) lies wholly in mapped
// memory (RAM through any alias, or the locked cache).
inline bool IsMapped(uint32_t address, size_t length) noexcept {
    return HostPointer(address, length) != nullptr;
}

} // namespace GuestFlat
