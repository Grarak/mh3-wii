// Checked guest RAM for the initial MH3 runtime. MMIO is intentionally fatal
// until the corresponding host service is implemented. No CPU interpretation.
#include "memory.h"
#include "video_interface.h"
#include "audio_interface.h"
#include "gx_host.h"
#include <array>
#include <cstring>
#include <cstdlib>
#include <sstream>

extern "C" void DumpHostStackTraceForRuntimeHelper();

namespace {
std::vector<uint8_t> mem1, mem2;
// MH3's LCEnable allocates all 512 32-byte locked L1 lines here.
// Keep this separate from MEM1/MEM2: masking this address as a RAM alias is wrong.
std::array<uint8_t, 0x4000> lockedCache{};
uint32_t sensorBarOutput = 0;
std::string accessMessage(uint32_t address, std::string_view reason) {
    std::ostringstream message;
    message << reason << " at guest 0x" << std::hex << address
            << ", translated function 0x" << RecompMod::CurrentTranslatedExecutionAddress();
    return message.str();
}
uint8_t* resolve(uint32_t address, size_t length) {
    if (address >= 0xE0000000 && address < 0xE0004000) {
        const auto offset = address - 0xE0000000;
        return length <= lockedCache.size() - offset ? lockedCache.data() + offset : nullptr;
    }
    uint32_t physical = address & 0x3FFFFFFF;
    // Only the hardware's physical, cached and uncached RAM aliases.
    if ((address >> 30) == 1) return nullptr;
    auto* region = &mem1;
    if (physical >= 0x10000000) {
        physical -= 0x10000000;
        region = &mem2;
    }
    if (physical > region->size() || length > region->size() - physical) return nullptr;
    return region->data() + physical;
}
template<class T> T read(uint32_t address) {
    if constexpr (std::is_same_v<T, uint32_t>) {
        uint32_t audioValue;
        if (ReadAudioRegister32(address, audioValue)) return audioValue;
        if ((address >> 30) != 1 && (address & 0x3FFFFFFF) == 0x0D0000C0)
            return sensorBarOutput;
    }
    if constexpr (std::is_same_v<T, uint16_t>) {
        uint16_t value;
        if (ReadVideoRegister16(address, value)) return value;
    }
    uint32_t gxValue;
    if (ReadGxRegister(address, sizeof(T), gxValue)) return static_cast<T>(gxValue);
    T value;
    std::memcpy(&value, Memory::GetPointer(address, sizeof(T)), sizeof(T));
    return MemoryInline::MaybeByteSwap(value);
}
template<class T> void write(uint32_t address, T value) {
    if constexpr (std::is_same_v<T, uint32_t>) {
        if (WriteAudioRegister32(address, value)) return;
        if ((address >> 30) != 1 && (address & 0x3FFFFFFF) == 0x0D0000C0) {
            // GPIOB_OUT bit 8 controls the sensor bar. Other output devices
            // require their own implementation; do not silently acknowledge them.
            if (value & ~0x100u) throw std::runtime_error("Unsupported GPIOB output device");
            sensorBarOutput = value;
            return;
        }
    }
    if ((address >> 30) != 1 && (address & 0x3FFFFF00) == 0x0C008000) {
        const auto encoded = MemoryInline::MaybeByteSwap(value);
        SubmitGuestGxBytes(reinterpret_cast<const uint8_t*>(&encoded), sizeof(T));
        return;
    }
    if constexpr (std::is_same_v<T, uint16_t>) {
        if (WriteVideoRegister16(address, value)) return;
    }
    if (WriteGxRegister(address, sizeof(T), static_cast<uint32_t>(value))) return;
    RecompMod::CheckExecutableWrite(address, sizeof(T), value);
    auto encoded = MemoryInline::MaybeByteSwap(value);
    std::memcpy(Memory::GetPointer(address, sizeof(T)), &encoded, sizeof(T));
}
}

namespace GuestFlat {
bool g_requiresCheckedAccess = true;
bool IsActive() { return false; }
}

Memory::AccessViolation::AccessViolation(uint32_t addr, size_t length, std::string_view reason)
    : std::runtime_error(accessMessage(addr, reason)), address_(addr), length_(length), reason_(reason) {}

void Memory::Init(size_t size) {
    ResetVideoInterface();
    ResetAudioInterface();
    ResetGxRegisters();
    mem1.assign(size, 0);
    mem2.assign(64 * 1024 * 1024, 0);
    lockedCache.fill(0);
    sensorBarOutput = 0;
    for (uint32_t i = 0; i < MemoryInline::kPageCount; ++i) {
        auto* pointer = resolve(i << MemoryInline::kPageShift, MemoryInline::kPageSize);
        MemoryInline::g_pageTable[i] = {pointer, pointer ? MemoryInline::kPageSize : 0};
    }
}
uint8_t* Memory::GetPointer(uint32_t address) { return GetPointer(address, 1); }
uint8_t* Memory::GetPointer(uint32_t address, size_t length) {
    auto* pointer = resolve(address, length);
    if (!pointer) {
        // Capture before stack unwinding; this avoids requiring an interactive
        // debugger authorization for routine hardware-service bring-up.
        if (std::getenv("MH3_TRACE_ERRORS")) DumpHostStackTraceForRuntimeHelper();
        throw AccessViolation(address, length, "Unmapped RAM or unimplemented MMIO");
    }
    return pointer;
}
bool Memory::Contains(uint32_t address, size_t length) { return resolve(address, length) != nullptr; }

namespace MemoryInline {
PageEntry g_pageTable[kPageCount]{};
uintptr_t g_fullPageBias[kPageCount]{};
uintptr_t g_fullReadablePageBias[kPageCount]{};
uintptr_t g_fullWritablePageBias[kPageCount]{};
const SparseWritablePageTable* g_sparseWritablePageTables[kPageCount]{};
uint8_t g_deferredReadCoveredPages[kPageCount]{};
bool ResolveDeferredReads(uint32_t, size_t) { return false; }
#define SCALAR(bits, type) \
type Read##bits##Slow(uint32_t a) { return read<type>(a); } \
void Write##bits##Slow(uint32_t a, type v) { write(a, v); }
SCALAR(8, uint8_t)
SCALAR(16, uint16_t)
SCALAR(32, uint32_t)
SCALAR(64, uint64_t)
#undef SCALAR
float ReadFloat32Slow(uint32_t a) { auto bits = read<uint32_t>(a); float v; std::memcpy(&v, &bits, 4); return v; }
double ReadFloat64Slow(uint32_t a) { auto bits = read<uint64_t>(a); double v; std::memcpy(&v, &bits, 8); return v; }
void WriteFloat32Slow(uint32_t a, double v) { write(a, ConvertPpcDoubleToSingleBits(v)); }
void WriteFloat64Slow(uint32_t a, double v) { uint64_t bits; std::memcpy(&bits, &v, 8); write(a, bits); }
template<class T> T ReadResolvedFallback(uint32_t a) { return read<T>(a); }
template<class T> void WriteResolvedFallback(uint32_t a, T v) { write(a, v); }
#define INSTANTIATE(type) \
template type ReadResolvedFallback<type>(uint32_t); \
template void WriteResolvedFallback<type>(uint32_t, type);
INSTANTIATE(uint8_t)
INSTANTIATE(uint16_t)
INSTANTIATE(uint32_t)
INSTANTIATE(uint64_t)
#undef INSTANTIATE
float ReadResolvedFallbackFloat32(uint32_t a) { return Memory::ReadFloat32(a); }
double ReadResolvedFallbackFloat64(uint32_t a) { return Memory::ReadFloat64(a); }
void WriteResolvedFallbackFloat32(uint32_t a, double v) { Memory::WriteFloat32(a, v); }
void WriteResolvedFallbackFloat64(uint32_t a, double v) { Memory::WriteFloat64(a, v); }
}

// FIFO scalar writes preserve the guest's big-endian command encoding.
#ifndef MH3_HAS_AURORA
void SubmitGuestGxBytes(const uint8_t*, size_t) { throw std::runtime_error("GX requires the Aurora-linked build"); }
#endif
extern "C" void GX_HLE_FIFO_WriteBurst(const uint8_t* data, uint32_t size) { SubmitGuestGxBytes(data, size); }
extern "C" void GX_HLE_FIFO_Write8(uint8_t v) { SubmitGuestGxBytes(&v, 1); }
extern "C" void GX_HLE_FIFO_Write16(uint16_t v) { const uint8_t b[]{uint8_t(v >> 8), uint8_t(v)}; SubmitGuestGxBytes(b, 2); }
extern "C" void GX_HLE_FIFO_Write32(uint32_t v) { const uint8_t b[]{uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; SubmitGuestGxBytes(b, 4); }
extern "C" void GX_HLE_FIFO_WriteFloat(float v) { uint32_t bits; std::memcpy(&bits, &v, 4); GX_HLE_FIFO_Write32(bits); }
