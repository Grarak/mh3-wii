#pragma once

// Guest memory access for translated code and the runtime.
//
// MemoryInline is what translated code uses: guest memory is mapped flat
// (guest_flat_memory.h), so an access is a byte swap around
// `*(T*)(kGuestBase + addr)` - no page table, no bias lookup, no check. An
// access to hardware or to any unmapped address faults and is reported.
//
// MemoryHw has the same helper names and is used by the translated functions
// listed in runtime/src/hardware_access.inc (the translator requalifies their
// MemoryInline:: and PPC_Psq* helpers): a hardware address goes to the device
// models (HardwareRead/HardwareWrite in memory.cpp), anything else is the
// same flat access.
//
// Memory::Read*/Write* is the host API for native runtime code: hardware goes
// to the device models, RAM (physical aliases included) is read directly, and
// anything else throws Memory::AccessViolation.
#include "guest_flat_memory.h"
#include "memory.h"
#include "recomp_mod_loader.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

// The GX FIFO (gather pipe) writes the translator lowers guest stores to.
// The window is the free tail of the GX worker's filling chunk
// (graphics_host.cpp): while it has room a write is a big-endian store into
// it; otherwise (no worker, GX synchronized, chunk full; `end` null when
// closed) it goes the full way, GX_HLE_FIFO_WriteFull. The translator's
// coalesced bursts (GX_HLE_FIFO_WriteBurst, a constant size) take the window
// too: a few stores instead of a call and a memcpy each.
struct GxFifoWindow {
    uint8_t* next;
    uint8_t* end;
};
extern GxFifoWindow gGxFifoWindow;
extern "C" {
void GX_HLE_FIFO_WriteFull(const uint8_t* data, uint32_t sizeBytes);
inline void GX_HLE_FIFO_WriteBytesInline(const uint8_t* bytes, uint32_t size) {
    GxFifoWindow& w = gGxFifoWindow;
    if (static_cast<size_t>(w.end - w.next) >= size) [[likely]] {
        std::memcpy(w.next, bytes, size);
        w.next += size;
        return;
    }
    GX_HLE_FIFO_WriteFull(bytes, size);
}
inline void GX_HLE_FIFO_WriteBurst(const uint8_t* data, uint32_t sizeBytes) {
    GX_HLE_FIFO_WriteBytesInline(data, sizeBytes);
}
inline void GX_HLE_FIFO_Write8(uint8_t val) { GX_HLE_FIFO_WriteBytesInline(&val, 1); }
inline void GX_HLE_FIFO_Write16(uint16_t val) {
    const uint16_t bytes = __builtin_bswap16(val);
    GX_HLE_FIFO_WriteBytesInline(reinterpret_cast<const uint8_t*>(&bytes), 2);
}
inline void GX_HLE_FIFO_Write32(uint32_t val) {
    const uint32_t bytes = __builtin_bswap32(val);
    GX_HLE_FIFO_WriteBytesInline(reinterpret_cast<const uint8_t*>(&bytes), 4);
}
inline void GX_HLE_FIFO_WriteFloat(float val) {
    uint32_t bits;
    std::memcpy(&bits, &val, 4);
    GX_HLE_FIFO_Write32(bits);
}
}

#define MKW_MEMORY_FORCE_INLINE __attribute__((always_inline)) inline
#define MKW_MEMORY_NO_INLINE __attribute__((noinline))
#define MKW_MEMORY_COLD __attribute__((cold))

namespace MemoryInline {

// Guest-address byte order. Distinct from isa/big_endian.h, which is the
// host-pointer codec; do not "unify" them.
inline uint16_t ByteSwap16(uint16_t value) { return __builtin_bswap16(value); }
inline uint32_t ByteSwap32(uint32_t value) { return __builtin_bswap32(value); }
inline uint64_t ByteSwap64(uint64_t value) { return __builtin_bswap64(value); }

template <typename T>
inline T MaybeByteSwap(T value) {
    if constexpr (sizeof(T) == 2) return static_cast<T>(ByteSwap16(static_cast<uint16_t>(value)));
    else if constexpr (sizeof(T) == 4) return static_cast<T>(ByteSwap32(static_cast<uint32_t>(value)));
    else if constexpr (sizeof(T) == 8) return static_cast<T>(ByteSwap64(static_cast<uint64_t>(value)));
    else return value;
}

// Gekko stfs conversion is a bit-level narrowing operation.  In particular it
// does not behave like a host double-to-float cast for values which were left
// in double precision, and it has hardware-tested handling for tiny values.
MKW_MEMORY_FORCE_INLINE uint32_t ConvertPpcDoubleToSingleBits(double value) {
    // A value that is exactly a single (every lfs result and single-precision
    // op) narrows to that single's bits, subnormals included. Written as a
    // cast round trip so the compiler folds it when the value is a widened
    // float, leaving no conversion at all; NaNs take the bit narrowing.
    const float single = static_cast<float>(value);
    if (static_cast<double>(single) == value) [[likely]]
    {
        uint32_t singleBits;
        std::memcpy(&singleBits, &single, sizeof(singleBits));
        return singleBits;
    }
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = static_cast<uint32_t>((bits >> 52) & 0x7FFu);
    // The subnormal-single arm applies to exactly the exponents 874..896; every
    // other exponent takes the plain sign/exponent/fraction narrowing below.
    // Inside that window the exponent field is nonzero, so the magnitude cannot
    // be zero and needs no separate test - the zero case (exponent 0) reaches
    // the narrowing exactly as it did when the two arms shared that test.
    if (exponent - 874u <= 22u) [[unlikely]]
    {
        uint32_t narrowed = static_cast<uint32_t>(
            0x80000000ULL | ((bits & 0x000FFFFFFFFFFFFFULL) >> 21));
        narrowed >>= (905u - exponent);
        narrowed |= static_cast<uint32_t>((bits >> 32) & 0x80000000ULL);
        return narrowed;
    }

    // Results below the documented conversion range are architecturally
    // undefined; this is the behavior measured on Gekko/Broadway hardware.
    return static_cast<uint32_t>(
        ((bits >> 32) & 0xC0000000ULL) | ((bits >> 29) & 0x3FFFFFFFULL));
}

MKW_MEMORY_FORCE_INLINE float PpcSingleBitsToFloat(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

#ifdef MH3_WRITE_WATCH
// Debug build (MH3_WRITE_WATCH): guest stores into [start, start + length)
// are reported with the translated function doing them. With
// MH3_WRITE_WATCH_LIST, only the listed words of the range: one bit per word
// from `start` in g_writeWatchWords.
inline uint32_t g_writeWatchStart = 0, g_writeWatchLength = 0;
inline const uint8_t* g_writeWatchWords = nullptr;
MKW_MEMORY_FORCE_INLINE bool WriteWatched(uint32_t address) {
    const uint32_t offset = address - g_writeWatchStart;
    if (offset >= g_writeWatchLength) return false;
    return !g_writeWatchWords || (g_writeWatchWords[offset >> 5] >> ((offset >> 2) & 7) & 1);
}
void RecordWatchedWrite(uint32_t address, uint32_t size, uint64_t value, const void* hostCaller);
// Out of line so its return address is the translated function.
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void NoteWatchedWrite(uint32_t address, uint32_t size, uint64_t value);
#endif

template <typename T>
MKW_MEMORY_FORCE_INLINE T FlatLoad(uint32_t address) {
    T value{};
    std::memcpy(&value, GuestFlat::Host(address), sizeof(T));
    return MaybeByteSwap(value);
}

template <typename T>
MKW_MEMORY_FORCE_INLINE void FlatStore(uint32_t address, T value) {
#ifdef MH3_WRITE_WATCH
    if (WriteWatched(address) || WriteWatched(address + sizeof(T) - 1)) [[unlikely]]
        NoteWatchedWrite(address, sizeof(T), static_cast<uint64_t>(value));
#endif
    const T swapped = MaybeByteSwap(value);
    std::memcpy(GuestFlat::Host(address), &swapped, sizeof(T));
}

MKW_MEMORY_FORCE_INLINE uint8_t FlatRead8(uint32_t a) { return FlatLoad<uint8_t>(a); }
MKW_MEMORY_FORCE_INLINE uint16_t FlatRead16(uint32_t a) { return FlatLoad<uint16_t>(a); }
MKW_MEMORY_FORCE_INLINE uint32_t FlatRead32(uint32_t a) { return FlatLoad<uint32_t>(a); }
MKW_MEMORY_FORCE_INLINE uint64_t FlatRead64(uint32_t a) { return FlatLoad<uint64_t>(a); }
MKW_MEMORY_FORCE_INLINE float FlatReadFloat32(uint32_t a) { return PpcSingleBitsToFloat(FlatLoad<uint32_t>(a)); }
MKW_MEMORY_FORCE_INLINE double FlatReadFloat64(uint32_t a) {
    const uint64_t bits = FlatLoad<uint64_t>(a);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

MKW_MEMORY_FORCE_INLINE void FlatWrite8(uint32_t a, uint8_t v) { FlatStore<uint8_t>(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWrite16(uint32_t a, uint16_t v) { FlatStore<uint16_t>(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWrite32(uint32_t a, uint32_t v) { FlatStore<uint32_t>(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWrite64(uint32_t a, uint64_t v) { FlatStore<uint64_t>(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteFloat32(uint32_t a, double v) { FlatStore<uint32_t>(a, ConvertPpcDoubleToSingleBits(v)); }
MKW_MEMORY_FORCE_INLINE void FlatWriteFloat64(uint32_t a, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    FlatStore<uint64_t>(a, bits);
}

// The translator's proven-RAM store family (r1-relative stack slots). With
// nothing to check on any store, it is the same access as FlatWrite*.
MKW_MEMORY_FORCE_INLINE void FlatWriteRam8(uint32_t a, uint8_t v) { FlatWrite8(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam16(uint32_t a, uint16_t v) { FlatWrite16(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam32(uint32_t a, uint32_t v) { FlatWrite32(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam64(uint32_t a, uint64_t v) { FlatWrite64(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRamFloat32(uint32_t a, double v) { FlatWriteFloat32(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRamFloat64(uint32_t a, double v) { FlatWriteFloat64(a, v); }

// Stack slots: ordinary guest memory.
inline uint8_t ReadStack8(uint32_t a) { return FlatRead8(a); }
inline uint16_t ReadStack16(uint32_t a) { return FlatRead16(a); }
inline uint32_t ReadStack32(uint32_t a) { return FlatRead32(a); }
inline uint64_t ReadStack64(uint32_t a) { return FlatRead64(a); }
inline void WriteStack8(uint32_t a, uint8_t v) { FlatWrite8(a, v); }
inline void WriteStack16(uint32_t a, uint16_t v) { FlatWrite16(a, v); }
inline void WriteStack32(uint32_t a, uint32_t v) { FlatWrite32(a, v); }
inline void WriteStack64(uint32_t a, uint64_t v) { FlatWrite64(a, v); }
inline void WriteStackFloat32(uint32_t a, double v) { FlatWriteFloat32(a, v); }
inline void WriteStackFloat64(uint32_t a, double v) { FlatWriteFloat64(a, v); }

// Resolved ranges: the translator resolves `base + minOffset` once and then
// addresses the range as host + constant. A null host (a range that wraps
// past 0xFFFFFFFF) sends each access through the guest address instead.
MKW_MEMORY_FORCE_INLINE uint8_t* ResolveRangeHost(uint32_t base, int32_t minOffset, uint32_t length,
                                                 bool, bool) {
    const uint32_t guestStart = base + static_cast<uint32_t>(minOffset);
    if (length == 0 || guestStart > UINT32_MAX - (length - 1)) [[unlikely]] return nullptr;
    return GuestFlat::Host(guestStart);
}

template <typename T>
MKW_MEMORY_FORCE_INLINE bool ReadResolvedScalar(uint8_t* host, uint32_t rangeOffset, T& outValue) {
    if (!host) return false;
    T value{};
    std::memcpy(&value, host + rangeOffset, sizeof(T));
    outValue = MaybeByteSwap(value);
    return true;
}

template <typename T>
MKW_MEMORY_FORCE_INLINE bool WriteResolvedScalar(uint8_t* host, uint32_t rangeOffset, T value) {
    if (!host) return false;
#ifdef MH3_WRITE_WATCH
    return false;  // every store goes through FlatStore's watch
#endif
    const T swapped = MaybeByteSwap(value);
    std::memcpy(host + rangeOffset, &swapped, sizeof(T));
    return true;
}

template <typename T>
MKW_MEMORY_FORCE_INLINE T ReadResolved(uint8_t* host, uint32_t rangeOffset, uint32_t address) {
    T value{};
    if (ReadResolvedScalar(host, rangeOffset, value)) return value;
    return FlatLoad<T>(address);
}

template <typename T>
MKW_MEMORY_FORCE_INLINE void WriteResolved(uint8_t* host, uint32_t rangeOffset, uint32_t address, T value) {
    if (!WriteResolvedScalar(host, rangeOffset, value)) FlatStore<T>(address, value);
}

MKW_MEMORY_FORCE_INLINE uint8_t ReadResolved8(uint8_t* r, uint32_t o, uint32_t a) { return ReadResolved<uint8_t>(r, o, a); }
MKW_MEMORY_FORCE_INLINE uint16_t ReadResolved16(uint8_t* r, uint32_t o, uint32_t a) { return ReadResolved<uint16_t>(r, o, a); }
MKW_MEMORY_FORCE_INLINE uint32_t ReadResolved32(uint8_t* r, uint32_t o, uint32_t a) { return ReadResolved<uint32_t>(r, o, a); }
MKW_MEMORY_FORCE_INLINE uint64_t ReadResolved64(uint8_t* r, uint32_t o, uint32_t a) { return ReadResolved<uint64_t>(r, o, a); }
MKW_MEMORY_FORCE_INLINE float ReadResolvedFloat32(uint8_t* r, uint32_t o, uint32_t a) {
    return PpcSingleBitsToFloat(ReadResolved<uint32_t>(r, o, a));
}
MKW_MEMORY_FORCE_INLINE double ReadResolvedFloat64(uint8_t* r, uint32_t o, uint32_t a) {
    const uint64_t bits = ReadResolved<uint64_t>(r, o, a);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
MKW_MEMORY_FORCE_INLINE void WriteResolved8(uint8_t* r, uint32_t o, uint32_t a, uint8_t v) { WriteResolved(r, o, a, v); }
MKW_MEMORY_FORCE_INLINE void WriteResolved16(uint8_t* r, uint32_t o, uint32_t a, uint16_t v) { WriteResolved(r, o, a, v); }
MKW_MEMORY_FORCE_INLINE void WriteResolved32(uint8_t* r, uint32_t o, uint32_t a, uint32_t v) { WriteResolved(r, o, a, v); }
MKW_MEMORY_FORCE_INLINE void WriteResolved64(uint8_t* r, uint32_t o, uint32_t a, uint64_t v) { WriteResolved(r, o, a, v); }
MKW_MEMORY_FORCE_INLINE void WriteResolvedFloat32(uint8_t* r, uint32_t o, uint32_t a, double v) {
    WriteResolved<uint32_t>(r, o, a, ConvertPpcDoubleToSingleBits(v));
}
MKW_MEMORY_FORCE_INLINE void WriteResolvedFloat64(uint8_t* r, uint32_t o, uint32_t a, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    WriteResolved<uint64_t>(r, o, a, bits);
}

// Two adjacent words (or halfwords) as one access. `valid` false (a null
// range) makes the caller fall back to the two single accesses.
struct ResolvedLoadPair {
    uint32_t first = 0;
    uint32_t second = 0;
    bool valid = false;
};

MKW_MEMORY_FORCE_INLINE ResolvedLoadPair ReadResolvedPair16(uint8_t* host, uint32_t rangeOffset) {
    uint32_t packed = 0;
    if (!ReadResolvedScalar(host, rangeOffset, packed)) return {};
    return {packed >> 16, packed & 0xFFFFu, true};
}

MKW_MEMORY_FORCE_INLINE ResolvedLoadPair ReadResolvedPair32(uint8_t* host, uint32_t rangeOffset) {
    uint64_t packed = 0;
    if (!ReadResolvedScalar(host, rangeOffset, packed)) return {};
    return {static_cast<uint32_t>(packed >> 32), static_cast<uint32_t>(packed), true};
}

MKW_MEMORY_FORCE_INLINE bool WriteResolvedPair16(uint8_t* host, uint32_t rangeOffset, uint32_t packed) {
    return WriteResolvedScalar(host, rangeOffset, packed);
}

MKW_MEMORY_FORCE_INLINE bool WriteResolvedPair32(uint8_t* host, uint32_t rangeOffset, uint64_t packed) {
    return WriteResolvedScalar(host, rangeOffset, packed);
}

// The paired-single helpers (isa/ppc_isa_quantized.h) address memory through
// these: GuestFlat::Host directly unless PsqHardware, a guest access otherwise.
constexpr bool PsqHardware(uint32_t) { return false; }
template <typename T> MKW_MEMORY_FORCE_INLINE T PsqLoad(uint32_t address) { return FlatLoad<T>(address); }
template <typename T> MKW_MEMORY_FORCE_INLINE void PsqStore(uint32_t address, T value) { FlatStore<T>(address, value); }

} // namespace MemoryInline

// The device models, in memory.cpp: MMIO registers, the GX FIFO and the EFB.
// An address no device claims throws Memory::AccessViolation.
template <typename T> MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD T HardwareRead(uint32_t address);
template <typename T> MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void HardwareWrite(uint32_t address, T value);

namespace MemoryHw {
// Helpers without memory access (byte swaps, float narrowing, the pair
// struct) are MemoryInline's.
using namespace MemoryInline;

template <typename T>
MKW_MEMORY_FORCE_INLINE T Load(uint32_t address) {
    if (GuestFlat::IsHardwareAddress(address)) [[unlikely]] return HardwareRead<T>(address);
    return MemoryInline::FlatLoad<T>(address);
}

template <typename T>
MKW_MEMORY_FORCE_INLINE void Store(uint32_t address, T value) {
    if (GuestFlat::IsHardwareAddress(address)) [[unlikely]] { HardwareWrite<T>(address, value); return; }
    MemoryInline::FlatStore<T>(address, value);
}

MKW_MEMORY_FORCE_INLINE uint8_t FlatRead8(uint32_t a) { return Load<uint8_t>(a); }
MKW_MEMORY_FORCE_INLINE uint16_t FlatRead16(uint32_t a) { return Load<uint16_t>(a); }
MKW_MEMORY_FORCE_INLINE uint32_t FlatRead32(uint32_t a) { return Load<uint32_t>(a); }
MKW_MEMORY_FORCE_INLINE uint64_t FlatRead64(uint32_t a) { return Load<uint64_t>(a); }
MKW_MEMORY_FORCE_INLINE float FlatReadFloat32(uint32_t a) { return PpcSingleBitsToFloat(Load<uint32_t>(a)); }
MKW_MEMORY_FORCE_INLINE double FlatReadFloat64(uint32_t a) {
    const uint64_t bits = Load<uint64_t>(a);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
MKW_MEMORY_FORCE_INLINE void FlatWrite8(uint32_t a, uint8_t v) { Store<uint8_t>(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWrite16(uint32_t a, uint16_t v) { Store<uint16_t>(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWrite32(uint32_t a, uint32_t v) { Store<uint32_t>(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWrite64(uint32_t a, uint64_t v) { Store<uint64_t>(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteFloat32(uint32_t a, double v) { Store<uint32_t>(a, ConvertPpcDoubleToSingleBits(v)); }
MKW_MEMORY_FORCE_INLINE void FlatWriteFloat64(uint32_t a, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    Store<uint64_t>(a, bits);
}
MKW_MEMORY_FORCE_INLINE void FlatWriteRam8(uint32_t a, uint8_t v) { FlatWrite8(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam16(uint32_t a, uint16_t v) { FlatWrite16(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam32(uint32_t a, uint32_t v) { FlatWrite32(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam64(uint32_t a, uint64_t v) { FlatWrite64(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRamFloat32(uint32_t a, double v) { FlatWriteFloat32(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRamFloat64(uint32_t a, double v) { FlatWriteFloat64(a, v); }

inline uint8_t ReadStack8(uint32_t a) { return FlatRead8(a); }
inline uint16_t ReadStack16(uint32_t a) { return FlatRead16(a); }
inline uint32_t ReadStack32(uint32_t a) { return FlatRead32(a); }
inline uint64_t ReadStack64(uint32_t a) { return FlatRead64(a); }
inline void WriteStack8(uint32_t a, uint8_t v) { FlatWrite8(a, v); }
inline void WriteStack16(uint32_t a, uint16_t v) { FlatWrite16(a, v); }
inline void WriteStack32(uint32_t a, uint32_t v) { FlatWrite32(a, v); }
inline void WriteStack64(uint32_t a, uint64_t v) { FlatWrite64(a, v); }
inline void WriteStackFloat32(uint32_t a, double v) { FlatWriteFloat32(a, v); }
inline void WriteStackFloat64(uint32_t a, double v) { FlatWriteFloat64(a, v); }

// No resolved ranges: a range may cover hardware, so every access goes
// through its guest address.
MKW_MEMORY_FORCE_INLINE uint8_t* ResolveRangeHost(uint32_t, int32_t, uint32_t, bool, bool) { return nullptr; }
MKW_MEMORY_FORCE_INLINE uint8_t ReadResolved8(uint8_t*, uint32_t, uint32_t a) { return FlatRead8(a); }
MKW_MEMORY_FORCE_INLINE uint16_t ReadResolved16(uint8_t*, uint32_t, uint32_t a) { return FlatRead16(a); }
MKW_MEMORY_FORCE_INLINE uint32_t ReadResolved32(uint8_t*, uint32_t, uint32_t a) { return FlatRead32(a); }
MKW_MEMORY_FORCE_INLINE uint64_t ReadResolved64(uint8_t*, uint32_t, uint32_t a) { return FlatRead64(a); }
MKW_MEMORY_FORCE_INLINE float ReadResolvedFloat32(uint8_t*, uint32_t, uint32_t a) { return FlatReadFloat32(a); }
MKW_MEMORY_FORCE_INLINE double ReadResolvedFloat64(uint8_t*, uint32_t, uint32_t a) { return FlatReadFloat64(a); }
MKW_MEMORY_FORCE_INLINE void WriteResolved8(uint8_t*, uint32_t, uint32_t a, uint8_t v) { FlatWrite8(a, v); }
MKW_MEMORY_FORCE_INLINE void WriteResolved16(uint8_t*, uint32_t, uint32_t a, uint16_t v) { FlatWrite16(a, v); }
MKW_MEMORY_FORCE_INLINE void WriteResolved32(uint8_t*, uint32_t, uint32_t a, uint32_t v) { FlatWrite32(a, v); }
MKW_MEMORY_FORCE_INLINE void WriteResolved64(uint8_t*, uint32_t, uint32_t a, uint64_t v) { FlatWrite64(a, v); }
MKW_MEMORY_FORCE_INLINE void WriteResolvedFloat32(uint8_t*, uint32_t, uint32_t a, double v) { FlatWriteFloat32(a, v); }
MKW_MEMORY_FORCE_INLINE void WriteResolvedFloat64(uint8_t*, uint32_t, uint32_t a, double v) { FlatWriteFloat64(a, v); }
MKW_MEMORY_FORCE_INLINE ResolvedLoadPair ReadResolvedPair16(uint8_t*, uint32_t) { return {}; }
MKW_MEMORY_FORCE_INLINE ResolvedLoadPair ReadResolvedPair32(uint8_t*, uint32_t) { return {}; }
MKW_MEMORY_FORCE_INLINE bool WriteResolvedPair16(uint8_t*, uint32_t, uint32_t) { return false; }
MKW_MEMORY_FORCE_INLINE bool WriteResolvedPair32(uint8_t*, uint32_t, uint64_t) { return false; }

// Paired singles: no direct host access for an address that may be hardware.
constexpr bool PsqHardware(uint32_t address) { return GuestFlat::IsHardwareAddress(address); }
template <typename T> MKW_MEMORY_FORCE_INLINE T PsqLoad(uint32_t address) { return Load<T>(address); }
template <typename T> MKW_MEMORY_FORCE_INLINE void PsqStore(uint32_t address, T value) { Store<T>(address, value); }
} // namespace MemoryHw

// Host API: hardware, RAM through any alias (physical included), or an
// AccessViolation.
namespace MemoryHost {
template <typename T>
inline T Load(uint32_t address) {
    if (const uint8_t* host = GuestFlat::HostPointer(address, sizeof(T))) {
        T value{};
        std::memcpy(&value, host, sizeof(T));
        return MemoryInline::MaybeByteSwap(value);
    }
    return HardwareRead<T>(address);
}

template <typename T>
inline void Store(uint32_t address, T value) {
    if (uint8_t* host = GuestFlat::HostPointer(address, sizeof(T))) {
        const T swapped = MemoryInline::MaybeByteSwap(value);
        std::memcpy(host, &swapped, sizeof(T));
        return;
    }
    HardwareWrite<T>(address, value);
}
} // namespace MemoryHost

inline uint8_t Memory::Read8(uint32_t addr) { return MemoryHost::Load<uint8_t>(addr); }
inline uint16_t Memory::Read16(uint32_t addr) { return MemoryHost::Load<uint16_t>(addr); }
inline uint32_t Memory::Read32(uint32_t addr) { return MemoryHost::Load<uint32_t>(addr); }
inline uint64_t Memory::Read64(uint32_t addr) { return MemoryHost::Load<uint64_t>(addr); }
inline float Memory::ReadFloat32(uint32_t addr) { return MemoryInline::PpcSingleBitsToFloat(Read32(addr)); }
inline double Memory::ReadFloat64(uint32_t addr) {
    const uint64_t bits = Read64(addr);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
inline void Memory::Write8(uint32_t addr, uint8_t val) { MemoryHost::Store<uint8_t>(addr, val); }
inline void Memory::Write16(uint32_t addr, uint16_t val) { MemoryHost::Store<uint16_t>(addr, val); }
inline void Memory::Write32(uint32_t addr, uint32_t val) { MemoryHost::Store<uint32_t>(addr, val); }
inline void Memory::Write64(uint32_t addr, uint64_t val) { MemoryHost::Store<uint64_t>(addr, val); }
inline void Memory::WriteFloat32(uint32_t addr, double val) {
    Write32(addr, MemoryInline::ConvertPpcDoubleToSingleBits(val));
}
inline void Memory::WriteFloat64(uint32_t addr, double val) {
    uint64_t bits;
    std::memcpy(&bits, &val, sizeof(bits));
    Write64(addr, bits);
}

#undef MKW_MEMORY_FORCE_INLINE
#undef MKW_MEMORY_NO_INLINE
#undef MKW_MEMORY_COLD
