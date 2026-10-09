// g3d resource accessors, RMHP08. C reconstructions of NW4R g3d's
// ResDic::operator[](int) (0x800627B4, g3d_resdict_ac.h) and the ResCommon
// reference check (0x800628A4), decompiled with Ghidra and checked against the
// Gekko disassembly.
//
// A ResDic holds a pointer to the dictionary: +4 s32 entry count, then a
// 16-byte root at +8 and real entries at +24 + idx * 16. Entry +12 is the
// signed data offset from the dictionary. A null
// dictionary gives null, an index outside [0, count - 1] panics, a zero offset
// gives null. The reference check panics on a null reference and returns it.
//
// The debug NW4R build calls a function for each piece (the null test, the
// reference check, the offset to pointer); the ports are a few loads. The
// registered bodies only move r3 and r4, so callers keep a precise call
// boundary.
#include "abi_bridge.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstdint>
#include <bit>

// The compatibility boundary maps guest RAM once; dictionary logic is the
// checked portable C11 reader shared with offline model validation.
uint32_t MH3NativeResDictionary(uint32_t reference, int32_t index);
uint32_t MH3NativeResReference(uint32_t reference);

namespace {

// 0x800628A4: ResCommon<T>::ref(), panicking on a null reference.
uint32_t ResCommonRefCheck(uint32_t res) {
    return MH3NativeResReference(res);
}

// 0x800627B4: ResDic::operator[](int idx), the data of entry idx.
uint32_t ResDicAt(uint32_t dic, uint32_t idx) {
    return MH3NativeResDictionary(dic, std::bit_cast<int32_t>(idx));
}

// The port, or with MH3_PORT_CHECK naming it the port checked against the
// original on every call (registers from the current context).
uint32_t Checked(const char* name, bool check, void (*original)(CpuContext*), void (*port)(CpuContext*), uint32_t r1, uint32_t r3, uint32_t r4, uint32_t (*direct)(uint32_t, uint32_t)) {
#ifdef MH3_DEVELOPMENT
    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        PortCheck::RunPure(c, name, PortCheck::Returns::Gpr3, original, port);
        return c->gpr[3];
    }
#else
    (void)name, (void)check, (void)original, (void)port, (void)r1;
#endif
    return direct(r3, r4);
}

} // namespace

extern "C" void func_800627B4(CpuContext* ctx);
extern "C" void func_800628A4(CpuContext* ctx);

static uint32_t ResDicAtDirect(uint32_t r3, uint32_t r4) {
    return ResDicAt(r3, r4);
}

static void ResDicAtPort(CpuContext* p) {
    p->gpr[3] = ResDicAt(p->gpr[3], p->gpr[4]);
}

static uint32_t ResDicAtChecked(uint32_t r1, uint32_t r3, uint32_t r4) {
    static const bool check = PortCheck::Enabled("ResDicLookup");
    return Checked("ResDicLookup", check, func_800627B4, ResDicAtPort, r1, r3, r4, ResDicAtDirect);
}

extern "C" void MH3_ResDicLookup(CpuContext* ctx) {
    ctx->gpr[3] = ResDicAtChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800627B4, MH3_ResDicLookup, "ResDicLookup");

static uint32_t RefCheckDirect(uint32_t r3, uint32_t) {
    return ResCommonRefCheck(r3);
}

static void RefCheckPort(CpuContext* p) {
    p->gpr[3] = ResCommonRefCheck(p->gpr[3]);
}

static uint32_t RefCheckChecked(uint32_t r1, uint32_t r3) {
    static const bool check = PortCheck::Enabled("ResCommonRefCheck");
    return Checked("ResCommonRefCheck", check, func_800628A4, RefCheckPort, r1, r3, 0, RefCheckDirect);
}

extern "C" void MH3_ResCommonRefCheck(CpuContext* ctx) {
    ctx->gpr[3] = RefCheckChecked(ctx->gpr[1], ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800628A4, MH3_ResCommonRefCheck, "ResCommonRefCheck");
