// Small NW4R accessors, RMHP08. C reconstructions from the disassembly of the
// debug-build helpers the effect and g3d code calls per particle and per node:
// each is a few loads behind a chain of calls with stack frames, which the
// ports replace with the loads.
//
//   0x800A8C5C EmitterResBody        r3 + 8 + *(r3 + 4), then that + 4 + *(that)
//   0x800AD294 ResEmitterNumPtclTrack   u16 at EmitterResBody(r3)
//   0x800ADED8 ResEmitterNumPtclInitTrack u16 at EmitterResBody(r3) + 2
//   0x800ADEB4 ResEmitterPtclTrackTable EmitterResBody(r3) + 4
//   0x800ADE30 ResEmitterGetPtclTrack   the table's u32 entry u16(r4), panics
//                                       when u16(r4) >= the track count
//   0x80070124 ResRefChecked         *r3, panics when it is null
//   0x80097DFC ResOffsetToPtr        *r3 + r4, 0 for a 0 offset; panics when
//                                    the pointer is not 4-aligned
//   0x80097F7C ResDicAtOffset14      ResDic lookup r4 in the dictionary at
//                                    offset +0x14 of *r3; panics when not
//                                    4-aligned
//   0x80062978, 0x8005D210 StoreAlignedPtr  *r3 = r4, panics when r4 is not
//                                    4-aligned; returns r3
//   0x80041EA4 Vec3Copy              3 floats r4 -> r3 (through lfs/stfs);
//                                    returns r3
//   0x80050D04 VectorSubtractWrapper r3 = r4 - r5 (paired-single ps_sub)
//
// The registered bodies read and write only argument registers, so callers
// keep a precise call boundary; MH3_PORT_CHECK=Name checks a port on every
// call (the return register, or the written memory).
#include "abi_bridge.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

[[noreturn]] void Panic(const char* what, uint32_t value) {
    char text[128];
    std::snprintf(text, sizeof text, "NW4R panic: %s (%08x)", what, value);
    throw std::runtime_error(text);
}

uint32_t Read32(uint32_t a) { return MemoryInline::FlatRead32(a); }

uint32_t EmitterResBody(uint32_t r) {
    const uint32_t a = r + Read32(r + 4) + 8;
    return a + Read32(a) + 4;
}
uint32_t NumPtclTrack(uint32_t r) { return MemoryInline::FlatRead16(EmitterResBody(r)); }
uint32_t NumPtclInitTrack(uint32_t r) { return MemoryInline::FlatRead16(EmitterResBody(r) + 2); }
uint32_t PtclTrackTable(uint32_t r) { return EmitterResBody(r) + 4; }
uint32_t GetPtclTrack(uint32_t r, uint32_t index) {
    const uint32_t body = EmitterResBody(r);
    const uint32_t i = index & 0xFFFF;
    if (i >= MemoryInline::FlatRead16(body)) Panic("ptcl track index out of range", index);
    return Read32(body + 4 + 4 * i);
}
uint32_t RefChecked(uint32_t r) {
    const uint32_t v = Read32(r);
    if (!v) Panic("null resource reference", r);
    return v;
}
uint32_t Aligned(uint32_t p) {
    if (p & 3) Panic("misaligned resource pointer", p);
    return p;
}
uint32_t OffsetToPtr(uint32_t r, uint32_t offset) { return offset ? Aligned(Read32(r) + offset) : 0; }
uint32_t DicAtOffset14(uint32_t r, uint32_t index) {
    const uint32_t dic = OffsetToPtr(r, Read32(RefChecked(r) + 0x14));
    if (!dic) return Aligned(0);
    const int32_t i = int32_t(index);
    if (!(i >= 0 && i <= int32_t(Read32(dic + 4)) - 1)) Panic("idx is out of bounds", index);
    const uint32_t offset = Read32(dic + (index + 1) * 16 + 0x14);
    return Aligned(offset ? dic + offset : 0);
}
void StoreAligned(uint32_t r, uint32_t p) {
    MemoryInline::FlatWrite32(r, p);
    Aligned(p);
}
void Vec3Copy(uint32_t dst, uint32_t src) {
    for (uint32_t k = 0; k < 12; k += 4) MemoryInline::FlatWriteFloat32(dst + k, MemoryInline::FlatReadFloat32(src + k));
}
float LoadFloat(uint32_t a) {
    const uint32_t bits = Read32(a);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
void StoreFloat(uint32_t a, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    MemoryInline::FlatWrite32(a, bits);
}
void VecSubtract(uint32_t out, uint32_t a, uint32_t b) {
    // PpcPairVec keeps ps0 in lane1; the z pair's ps1 is the psq_l W=1 1.0.
    const PpcPairVec xy = PpcSubPairInline(PpcPairVec{LoadFloat(a + 4), LoadFloat(a)},
                                           PpcPairVec{LoadFloat(b + 4), LoadFloat(b)});
    const PpcPairVec z = PpcSubPairInline(PpcPairVec{1.0f, LoadFloat(a + 8)}, PpcPairVec{1.0f, LoadFloat(b + 8)});
    StoreFloat(out, xy.lane1);
    StoreFloat(out + 4, xy.lane0);
    StoreFloat(out + 8, z.lane1);
}

} // namespace

// A port returning r3 from r3 (and r4), checked with RunPure.
#ifdef MH3_DEVELOPMENT
#define MH3_PURE_CHECK(Name, Addr)                                                                             \
    static const bool check = PortCheck::Enabled(#Name);                                                       \
    if (check) [[unlikely]] {                                                                                  \
        CpuContext* c = TryGetCpuContext();                                                                    \
        c->gpr[1] = r1;                                                                                        \
        c->gpr[3] = r3;                                                                                        \
        c->gpr[4] = r4;                                                                                        \
        PortCheck::RunPure(c, #Name, PortCheck::Returns::Gpr3, func_##Addr, Name##Port);                     \
        return c->gpr[3];                                                                                      \
    }
#else
#define MH3_PURE_CHECK(Name, Addr) (void)r1;
#endif

#define MH3_ACCESSOR1(Addr, Name, Fn)                                                                          \
    extern "C" void func_##Addr(CpuContext* ctx);                                                              \
    static void Name##Port(CpuContext* p) { p->gpr[3] = Fn(p->gpr[3]); }                                      \
    static uint32_t Name##Checked(uint32_t r1, uint32_t r3) {                                                  \
        [[maybe_unused]] const uint32_t r4 = 0;                                                                \
        MH3_PURE_CHECK(Name, Addr)                                                                             \
        return Fn(r3);                                                                                         \
    }

#define MH3_ACCESSOR2(Addr, Name, Fn)                                                                          \
    extern "C" void func_##Addr(CpuContext* ctx);                                                              \
    static void Name##Port(CpuContext* p) { p->gpr[3] = Fn(p->gpr[3], p->gpr[4]); }                           \
    static uint32_t Name##Checked(uint32_t r1, uint32_t r3, uint32_t r4) {                                     \
        MH3_PURE_CHECK(Name, Addr)                                                                             \
        return Fn(r3, r4);                                                                                     \
    }

MH3_ACCESSOR1(800A8C5C, EmitterResBody, EmitterResBody)
MH3_ACCESSOR1(800AD294, ResEmitterNumPtclTrack, NumPtclTrack)
MH3_ACCESSOR1(800ADED8, ResEmitterNumPtclInitTrack, NumPtclInitTrack)
MH3_ACCESSOR1(800ADEB4, ResEmitterPtclTrackTable, PtclTrackTable)
MH3_ACCESSOR2(800ADE30, ResEmitterGetPtclTrack, GetPtclTrack)
MH3_ACCESSOR1(80070124, ResRefChecked, RefChecked)
MH3_ACCESSOR2(80097DFC, ResOffsetToPtr, OffsetToPtr)
MH3_ACCESSOR2(80097F7C, ResDicAtOffset14, DicAtOffset14)

extern "C" void MH3_EmitterResBody(CpuContext* ctx) { ctx->gpr[3] = EmitterResBodyChecked(ctx->gpr[1], ctx->gpr[3]); }
REGISTER_NATIVE_FUNCTION_AS(0x800A8C5C, MH3_EmitterResBody, "EmitterResBody");
extern "C" void MH3_ResEmitterNumPtclTrack(CpuContext* ctx) { ctx->gpr[3] = ResEmitterNumPtclTrackChecked(ctx->gpr[1], ctx->gpr[3]); }
REGISTER_NATIVE_FUNCTION_AS(0x800AD294, MH3_ResEmitterNumPtclTrack, "ResEmitterNumPtclTrack");
extern "C" void MH3_ResEmitterNumPtclInitTrack(CpuContext* ctx) { ctx->gpr[3] = ResEmitterNumPtclInitTrackChecked(ctx->gpr[1], ctx->gpr[3]); }
REGISTER_NATIVE_FUNCTION_AS(0x800ADED8, MH3_ResEmitterNumPtclInitTrack, "ResEmitterNumPtclInitTrack");
extern "C" void MH3_ResEmitterPtclTrackTable(CpuContext* ctx) { ctx->gpr[3] = ResEmitterPtclTrackTableChecked(ctx->gpr[1], ctx->gpr[3]); }
REGISTER_NATIVE_FUNCTION_AS(0x800ADEB4, MH3_ResEmitterPtclTrackTable, "ResEmitterPtclTrackTable");
extern "C" void MH3_ResEmitterGetPtclTrack(CpuContext* ctx) { ctx->gpr[3] = ResEmitterGetPtclTrackChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]); }
REGISTER_NATIVE_FUNCTION_AS(0x800ADE30, MH3_ResEmitterGetPtclTrack, "ResEmitterGetPtclTrack");
extern "C" void MH3_ResRefChecked(CpuContext* ctx) { ctx->gpr[3] = ResRefCheckedChecked(ctx->gpr[1], ctx->gpr[3]); }
REGISTER_NATIVE_FUNCTION_AS(0x80070124, MH3_ResRefChecked, "ResRefChecked");
extern "C" void MH3_ResOffsetToPtr(CpuContext* ctx) { ctx->gpr[3] = ResOffsetToPtrChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]); }
REGISTER_NATIVE_FUNCTION_AS(0x80097DFC, MH3_ResOffsetToPtr, "ResOffsetToPtr");
extern "C" void MH3_ResDicAtOffset14(CpuContext* ctx) { ctx->gpr[3] = ResDicAtOffset14Checked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]); }
REGISTER_NATIVE_FUNCTION_AS(0x80097F7C, MH3_ResDicAtOffset14, "ResDicAtOffset14");

// Ports that write guest memory, checked with RunOutput; r3 is returned as
// it came.
#ifdef MH3_DEVELOPMENT
#define MH3_OUTPUT_CHECK(Name, Addr, Out, Bytes)                                                               \
    static const bool check = PortCheck::Enabled(#Name);                                                       \
    if (check) [[unlikely]] {                                                                                  \
        CpuContext* c = TryGetCpuContext();                                                                    \
        c->gpr[1] = r1;                                                                                        \
        c->gpr[3] = r3;                                                                                        \
        c->gpr[4] = r4;                                                                                        \
        c->gpr[5] = r5;                                                                                        \
        PortCheck::RunOutput(c, #Name, func_##Addr, Name##Port, {{Out, Bytes}});                               \
        return;                                                                                                \
    }
#else
#define MH3_OUTPUT_CHECK(Name, Addr, Out, Bytes) (void)r1;
#endif

#define MH3_STORE2(Addr, Name, Call, Out, Bytes)                                                               \
    extern "C" void func_##Addr(CpuContext* ctx);                                                              \
    static void Name##Port(CpuContext* p) {                                                                    \
        [[maybe_unused]] const uint32_t r3 = p->gpr[3], r4 = p->gpr[4], r5 = p->gpr[5];                        \
        Call;                                                                                                  \
    }                                                                                                          \
    static void Name##Checked(uint32_t r1, uint32_t r3, uint32_t r4, [[maybe_unused]] uint32_t r5) {           \
        MH3_OUTPUT_CHECK(Name, Addr, Out, Bytes)                                                               \
        Call;                                                                                                  \
    }

MH3_STORE2(80062978, StoreAlignedPtr, StoreAligned(r3, r4), r3, 4)
extern "C" void MH3_StoreAlignedPtr(CpuContext* ctx) { StoreAlignedPtrChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], 0); }
REGISTER_NATIVE_FUNCTION_AS(0x80062978, MH3_StoreAlignedPtr, "StoreAlignedPtr");

MH3_STORE2(8005D210, StoreAlignedPtr2, StoreAligned(r3, r4), r3, 4)
extern "C" void MH3_StoreAlignedPtr2(CpuContext* ctx) {
    StoreAlignedPtr2Checked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x8005D210, MH3_StoreAlignedPtr2, "StoreAlignedPtr2");

MH3_STORE2(80041EA4, Vec3Copy, Vec3Copy(r3, r4), r3, 12)
extern "C" void MH3_Vec3Copy(CpuContext* ctx) { Vec3CopyChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], 0); }
REGISTER_NATIVE_FUNCTION_AS(0x80041EA4, MH3_Vec3Copy, "Vec3Copy");

MH3_STORE2(80050D04, VectorSubtractWrapper, VecSubtract(r3, r4, r5), r3, 12)
extern "C" void MH3_VectorSubtractWrapper(CpuContext* ctx) {
    VectorSubtractWrapperChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80050D04, MH3_VectorSubtractWrapper, "VectorSubtractWrapper");
