#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

class Memory {
  public:
    static constexpr size_t kMem1Size = 24u * 1024u * 1024u;
    static constexpr size_t kMem2Size = 64u * 1024u * 1024u;
    static constexpr uint32_t kMem1PhysicalBase = 0x00000000u;
    static constexpr uint32_t kMem1CachedBase = 0x80000000u;
    static constexpr uint32_t kMem1UncachedBase = 0xC0000000u;
    static constexpr uint32_t kMem2PhysicalBase = 0x10000000u;
    static constexpr uint32_t kMem2CachedBase = 0x90000000u;
    static constexpr uint32_t kMem2UncachedBase = 0xD0000000u;
    static constexpr uint32_t kMem2PhysicalEnd = kMem2PhysicalBase + static_cast<uint32_t>(kMem2Size);
    static constexpr uint32_t kMem2CachedEnd = kMem2CachedBase + static_cast<uint32_t>(kMem2Size);
    static constexpr uint32_t kMem2UncachedEnd = kMem2UncachedBase + static_cast<uint32_t>(kMem2Size);

    class AccessViolation : public std::runtime_error {
      public:
        AccessViolation(uint32_t address, size_t length, std::string_view reason);

        uint32_t address() const noexcept { return address_; }
        size_t length() const noexcept { return length_; }
        std::string_view reason() const noexcept { return reason_; }

      private:
        uint32_t address_ = 0;
        size_t length_ = 0;
        std::string reason_;
    };

    // Maps fresh, zeroed guest memory (guest_flat_memory.h). MEM1 is always
    // 24 MiB; the size is checked, not configurable.
    static void Init(size_t mem1Size);

    static uint8_t Read8(uint32_t addr);
    static uint16_t Read16(uint32_t addr);
    static uint32_t Read32(uint32_t addr);
    static uint64_t Read64(uint32_t addr);
    static float ReadFloat32(uint32_t addr);
    static double ReadFloat64(uint32_t addr);
    static void Write8(uint32_t addr, uint8_t val);
    static void Write16(uint32_t addr, uint16_t val);
    static void Write32(uint32_t addr, uint32_t val);
    static void Write64(uint32_t addr, uint64_t val);
    static void WriteFloat32(uint32_t addr, double val);
    static void WriteFloat64(uint32_t addr, double val);

    // Guest RAM through the flat window, unchecked (memory_access.h); the
    // hardware registers (MMIO, the GX FIFO, the EFB) only through these.
    static uint16_t ReadHardware16(uint32_t addr);
    static uint32_t ReadHardware32(uint32_t addr);
    static void WriteHardware16(uint32_t addr, uint16_t val);
    static void WriteHardware32(uint32_t addr, uint32_t val);

    // Read32 for a caller that recovers from an address no RAM or device
    // handles: false where Read32 raises an access violation.
    static bool TryRead32(uint32_t addr, uint32_t& value);

    // The message of the access violation an unmapped access at addr raises.
    static std::string UnmappedMessage(uint32_t addr);

    static uint8_t* GetPointer(uint32_t addr);
    static uint8_t* GetPointer(uint32_t addr, size_t length);
    static bool Contains(uint32_t addr, size_t length = 1);
};

// The translated-code access layer is kept separately from the public memory API.
#include "memory_access.h"
