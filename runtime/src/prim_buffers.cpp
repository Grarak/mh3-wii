// The game's per-frame primitive buffers (prim_init_all), RMHP08, native.
// Two groups of 0x34-byte entries, handed out in order each frame and reset
// all at once by prim_init_all; the field and village tasks call it every
// frame. The translated reset paid a folded guest address and a byte swap per
// store, 1,152 entries (128 + 1,024 in Alatreon) a frame; the port writes the
// big-endian values straight through one host pointer per group, and only to
// the entries handed out since the group's last reset.
//
//   0x80796E60 + 4g  entry array of group g (r13 - 17536)
//   0x80796E68 + 4g  entries handed out this frame (r13 - 17528)
//   0x80793920 + 2g  u16 capacity (r13 - 31168)
//   0x80798910       two floats each reset entry starts with (r2 - 30288)
//
//   0x800E3CF4 PrimReset(entry)  +0x18, +0x20 = 0; +0x1E u16 = 0; +8, +9 = 0;
//                                +0x24, +0x28 = the two floats
//   0x800E3C80 PrimAlloc(group)  the next entry, reset, or 0 when full
//   0x800E3D20 PrimResetGroup(group)  count = 0, every entry reset
//   0x800E3DA4 prim_init_all     both groups
//
// The floats are copied as raw words, as lfs/stfs copies them (a signalling
// NaN included). MH3_PORT_CHECK=PrimBuffers checks the ports against the
// originals.
#include "abi_bridge.h"
#include "guest_flat_memory.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>
#include <cstring>

extern "C" void func_800E3CF4(CpuContext* ctx);
extern "C" void func_800E3C80(CpuContext* ctx);
extern "C" void func_800E3D20(CpuContext* ctx);
extern "C" void func_800E3DA4(CpuContext* ctx);

namespace {

using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatWrite32;

constexpr uint32_t kEntries = 0x80796E60;
constexpr uint32_t kUsed = 0x80796E68;
constexpr uint32_t kCapacity = 0x80793920;
constexpr uint32_t kResetFloats = 0x80798910;
constexpr uint32_t kEntryBytes = 0x34;

// The entry at `p` (host), with the reset floats' guest words `floats`.
inline void Reset(uint8_t* p, const uint8_t (&floats)[8]) {
    const uint32_t zero = 0;
    std::memcpy(p + 0x18, &zero, 4);
    std::memcpy(p + 0x20, &zero, 4);
    std::memcpy(p + 0x1E, &zero, 2);
    p[8] = 0;
    p[9] = 0;
    std::memcpy(p + 0x24, floats, 8);
}

inline void ResetFloats(uint8_t (&floats)[8]) {
    std::memcpy(floats, GuestFlat::Host(kResetFloats), 8);
}

void ResetEntry(uint32_t entry) {
    uint8_t floats[8];
    ResetFloats(floats);
    Reset(GuestFlat::Host(entry), floats);
}

uint32_t Alloc(uint32_t group) {
    const uint32_t g = group & 0xFF;
    const uint32_t used = FlatRead32(kUsed + 4 * g);

    if (used >= FlatRead16(kCapacity + 2 * g))
        return 0;

    const uint32_t entry = FlatRead32(kEntries + 4 * g) + used * kEntryBytes;
    FlatWrite32(kUsed + 4 * g, used + 1);
    ResetEntry(entry);
    return entry;
}

// Whether the entry at `p` holds what Reset writes.
bool InResetState(const uint8_t* p, const uint8_t (&floats)[8]) {
    static constexpr uint8_t zero[4] = {};
    return !std::memcmp(p + 0x18, zero, 4) && !std::memcmp(p + 0x20, zero, 4) && !std::memcmp(p + 0x1E, zero, 2) && !p[8] && !p[9] && !std::memcmp(p + 0x24, floats, 8);
}

// The array and capacity each group's last reset covered. Only the entries
// handed out since then (below the group's count) can have changed: Alloc
// resets an entry as it hands it out, nothing writes past the count, and the
// scene walk reads below it only. So a reset of the same array clears those
// (Alatreon: about 40 of 1,152 a frame) and leaves the rest in the reset state
// the original writes. A new array, or one the game allocated anew at the same
// address (its first untouched or last entry not in the reset state), is
// reset whole.
struct Covered {
    uint32_t entries = 0, capacity = 0;
};

Covered covered[2];

void ResetGroup(uint32_t group) {
    const uint32_t g = group & 0xFF;
    const uint32_t entries = FlatRead32(kEntries + 4 * g);
    const uint32_t used = FlatRead32(kUsed + 4 * g);
    FlatWrite32(kUsed + 4 * g, 0);
    const uint32_t capacity = FlatRead16(kCapacity + 2 * g);
    uint32_t count = capacity;

    if (!capacity)
        return;

    uint8_t floats[8];
    ResetFloats(floats);
    uint8_t* p = GuestFlat::Host(entries);

    if (g < 2) {
        const uint32_t touched = used < capacity ? used : capacity;

        if (covered[g].entries == entries && covered[g].capacity == capacity && InResetState(p + (capacity - 1) * kEntryBytes, floats) &&
            (touched == capacity || InResetState(p + touched * kEntryBytes, floats)))
            count = touched;

        covered[g] = {entries, capacity};
    }

    if (!count)
        return;

    for (uint32_t i = 0; i < count; ++i, p += kEntryBytes)
        Reset(p, floats);
}

void ResetAll() {
    ResetGroup(0);
    ResetGroup(1);
}

void ResetEntryPort(CpuContext* ctx) {
    ResetEntry(ctx->gpr[3]);
}

void AllocPort(CpuContext* ctx) {
    ctx->gpr[3] = Alloc(ctx->gpr[3]);
}

void ResetGroupPort(CpuContext* ctx) {
    ResetGroup(ctx->gpr[3]);
}

void ResetAllPort(CpuContext*) {
    ResetAll();
}

#ifdef MH3_DEVELOPMENT
// The originals and ports from the same RAM snapshot (rate-limited), with the
// registers the bodies read.
bool Check(uint32_t r1, uint32_t r3, void (*original)(CpuContext*), void (*port)(CpuContext*)) {
    const bool check = PortCheck::Enabled("PrimBuffers");

    if (!check) [[likely]]
        return false;
    CpuContext* c = TryGetCpuContext();
    c->gpr[1] = r1;
    c->gpr[3] = r3;
    PortCheck::Run(c, "PrimBuffers", original, port);
    return true;
}
#define MH3_PRIM_CHECK(Original, Port, Result)                                                                                                                                                         \
    if (Check(r1, r3, Original, Port)) [[unlikely]]                                                                                                                                                    \
        return Result;
#else
#define MH3_PRIM_CHECK(Original, Port, Result) (void)r1;
#endif

void ResetEntryChecked(uint32_t r1, uint32_t r3) {
    MH3_PRIM_CHECK(func_800E3CF4, ResetEntryPort, )
    ResetEntry(r3);
}

uint32_t AllocChecked(uint32_t r1, uint32_t r3) {
    MH3_PRIM_CHECK(func_800E3C80, AllocPort, TryGetCpuContext()->gpr[3])
    return Alloc(r3);
}

void ResetGroupChecked(uint32_t r1, uint32_t r3) {
    MH3_PRIM_CHECK(func_800E3D20, ResetGroupPort, )
    ResetGroup(r3);
}

void ResetAllChecked(uint32_t r1, uint32_t r3) {
    MH3_PRIM_CHECK(func_800E3DA4, ResetAllPort, )
    (void)r3;
    ResetAll();
}

} // namespace

extern "C" void MH3_PrimReset(CpuContext* ctx) {
    ResetEntryChecked(ctx->gpr[1], ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E3CF4, MH3_PrimReset, "PrimReset");

extern "C" void MH3_PrimAlloc(CpuContext* ctx) {
    ctx->gpr[3] = AllocChecked(ctx->gpr[1], ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E3C80, MH3_PrimAlloc, "PrimAlloc");

extern "C" void MH3_PrimResetGroup(CpuContext* ctx) {
    ResetGroupChecked(ctx->gpr[1], ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E3D20, MH3_PrimResetGroup, "PrimResetGroup");

extern "C" void MH3_PrimInitAll(CpuContext* ctx) {
    ResetAllChecked(ctx->gpr[1], ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E3DA4, MH3_PrimInitAll, "prim_init_all");
