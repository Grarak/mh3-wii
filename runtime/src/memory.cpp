// Guest memory: the flat window (guest_flat_memory.cpp) for RAM, and the
// device models for hardware. An MMIO access no device claims is fatal until
// the corresponding host service is implemented. No CPU interpretation.
#include "memory.h"
#include "video_interface.h"
#include "audio_interface.h"
#include "gx_host.h"
#include "gx_panic.h"
#include "efb_host.h"
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <type_traits>

namespace {
uint32_t sensorBarOutput = 0;
std::string accessMessage(uint32_t address, std::string_view reason) {
    std::ostringstream message;
    message << reason << " at guest 0x" << std::hex << address << ", translated function 0x" << RecompMod::CurrentTranslatedExecutionAddress();
    return message.str();
}

[[noreturn]] void Unmapped(uint32_t address, size_t length) {
    throw Memory::AccessViolation(address, length, "Unmapped RAM or unimplemented MMIO");
}

bool IsSensorBar(uint32_t address) {
    return (address >> 30) != 1 && (address & 0x3FFFFFFF) == 0x0D0000C0;
}
} // namespace

template <typename T>
T HardwareRead(uint32_t address) {
    if constexpr (std::is_same_v<T, uint32_t>) {
        uint32_t efbValue;

        if (ReadEfbColor32(address, efbValue))
            return efbValue;

        uint32_t audioValue;

        if (ReadAudioRegister32(address, audioValue))
            return audioValue;

        if (IsSensorBar(address))
            return sensorBarOutput;
    }

    if constexpr (std::is_same_v<T, uint16_t>) {
        uint16_t value;

        if (ReadVideoRegister16(address, value))
            return value;
    }

    uint32_t gxValue;

    if (ReadGxRegister(address, sizeof(T), gxValue))
        return static_cast<T>(gxValue);

    Unmapped(address, sizeof(T));
}

template <typename T>
void HardwareWrite(uint32_t address, T value) {
    if constexpr (std::is_same_v<T, uint32_t>) {
        if (WriteEfbColor32(address, value))
            return;

        if (WriteAudioRegister32(address, value))
            return;

        if (IsSensorBar(address)) {
            // GPIOB_OUT bit 8 controls the sensor bar. Other output devices
            // require their own implementation; do not silently acknowledge them.
            if (value & ~0x100u)
                throw std::runtime_error("Unsupported GPIOB output device");

            sensorBarOutput = value;
            return;
        }
    }

    if ((address >> 30) != 1 && (address & 0x3FFFFF00) == 0x0C008000) {
        const auto encoded = MemoryInline::MaybeByteSwap(value);

        if constexpr (sizeof(T) <= 4)
            SubmitGuestGxFixed<sizeof(T)>(reinterpret_cast<const uint8_t*>(&encoded));
        else
            SubmitGuestGxBytes(reinterpret_cast<const uint8_t*>(&encoded), sizeof(T));

        return;
    }

    if constexpr (std::is_same_v<T, uint16_t>) {
        if (WriteVideoRegister16(address, value))
            return;
    }

    if (WriteGxRegister(address, sizeof(T), static_cast<uint32_t>(value)))
        return;

    Unmapped(address, sizeof(T));
}

template uint8_t HardwareRead<uint8_t>(uint32_t);
template uint16_t HardwareRead<uint16_t>(uint32_t);
template uint32_t HardwareRead<uint32_t>(uint32_t);
template uint64_t HardwareRead<uint64_t>(uint32_t);
template void HardwareWrite<uint8_t>(uint32_t, uint8_t);
template void HardwareWrite<uint16_t>(uint32_t, uint16_t);
template void HardwareWrite<uint32_t>(uint32_t, uint32_t);
template void HardwareWrite<uint64_t>(uint32_t, uint64_t);

Memory::AccessViolation::AccessViolation(uint32_t addr, size_t length, std::string_view reason) : std::runtime_error(accessMessage(addr, reason)), address_(addr), length_(length), reason_(reason) {}

void Memory::Init(size_t size) {
    if (size != kMem1Size)
        throw std::runtime_error("MEM1 is 24 MiB");

    ResetVideoInterface();
    ResetAudioInterface();
    ResetGxRegisters();
    GuestFlat::Initialize();
    sensorBarOutput = 0;
}

uint8_t* Memory::GetPointer(uint32_t address) {
    return GetPointer(address, 1);
}

uint8_t* Memory::GetPointer(uint32_t address, size_t length) {
    if (auto* pointer = GuestFlat::HostPointer(address, length))
        return pointer;

    Unmapped(address, length);
}

bool Memory::Contains(uint32_t address, size_t length) {
    return GuestFlat::IsMapped(address, length);
}

// FIFO scalar writes preserve the guest's big-endian command encoding.
#ifndef MH3_HAS_AURORA
void SubmitGuestGxBytes(const uint8_t*, size_t) {
    PanicLegacyGx("GX FIFO write");
}

template <size_t N>
void SubmitGuestGxFixed(const uint8_t* data) {
    SubmitGuestGxBytes(data, N);
}
#ifdef MH3_DEVELOPMENT
void CaptureGuestGx(std::vector<uint8_t>*) {}
#endif
#endif
GxFifoWindow gGxFifoWindow{};
extern "C" void GX_HLE_FIFO_WriteFull(const uint8_t*, uint32_t) {
    PanicLegacyGx("GX FIFO full write");
}
