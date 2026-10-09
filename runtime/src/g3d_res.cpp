// g3d resource accessors, RMHP08. C reconstructions of NW4R g3d's
// ResDic::operator[](int) (0x800627B4, g3d_resdict_ac.h) and the ResCommon
// reference check (0x800628A4), decompiled with Ghidra and checked against the
// Gekko disassembly.
//
// A ResDic holds a pointer to the dictionary: +4 s32 entry count, then 16-byte
// entries from +0x10 whose +4 is the data's offset from the dictionary. Entry
// idx is at +0x10 + (idx + 1) * 16 (entry 0 is the tree's root). A null
// dictionary gives null, an index outside [0, count - 1] panics, a zero offset
// gives null. The reference check panics on a null reference and returns it.
//
// The debug NW4R build calls a function for each piece (the null test, the
// reference check, the offset to pointer); the ports are a few loads. The
// registered bodies only move r3 and r4, so callers keep a precise call
// boundary.
#include "abi_bridge.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace {

[[noreturn]] void Panic(const char* file, const char* what, uint32_t value) {
    char text[128];
    std::snprintf(text, sizeof text, "NW4R panic (%s): %s (%08x)", file, what, value);
    throw std::runtime_error(text);
}

// 0x800628A4: ResCommon<T>::ref(), panicking on a null reference.
uint32_t ResCommonRefCheck(uint32_t res) {
    const uint32_t data = MemoryInline::FlatRead32(res);
    if (!data) Panic("g3d_rescommon.h", "NW4R:Failed assertion IsValid()", res);
    return data;
}

// 0x800627B4: ResDic::operator[](int idx), the data of entry idx.
uint32_t ResDicAt(uint32_t dic, uint32_t idx) {
    const uint32_t data = MemoryInline::FlatRead32(dic);
    if (!data) return 0;
    const int32_t index = int32_t(idx);
    if (!(index >= 0 && index <= int32_t(MemoryInline::FlatRead32(data + 4)) - 1))
        Panic("g3d_resdict_ac.h", "idx is out of bounds", idx);
    const uint32_t offset = MemoryInline::FlatRead32(data + (idx + 1) * 16 + 0x14);
    return offset ? data + offset : 0;
}

// The port, or with MH3_PORT_CHECK naming it the port checked against the
// original on every call (registers from the current context).
uint32_t Checked(const char* name, bool check, void (*original)(CpuContext*), void (*port)(CpuContext*),
                 uint32_t r1, uint32_t r3, uint32_t r4, uint32_t (*direct)(uint32_t, uint32_t)) {
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

static uint32_t ResDicAtDirect(uint32_t r3, uint32_t r4) { return ResDicAt(r3, r4); }
static void ResDicAtPort(CpuContext* p) { p->gpr[3] = ResDicAt(p->gpr[3], p->gpr[4]); }
static uint32_t ResDicAtChecked(uint32_t r1, uint32_t r3, uint32_t r4) {
    static const bool check = PortCheck::Enabled("ResDicLookup");
    return Checked("ResDicLookup", check, func_800627B4, ResDicAtPort, r1, r3, r4, ResDicAtDirect);
}
extern "C" void MH3_ResDicLookup(CpuContext* ctx) {
    ctx->gpr[3] = ResDicAtChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800627B4, MH3_ResDicLookup, "ResDicLookup");

static uint32_t RefCheckDirect(uint32_t r3, uint32_t) { return ResCommonRefCheck(r3); }
static void RefCheckPort(CpuContext* p) { p->gpr[3] = ResCommonRefCheck(p->gpr[3]); }
static uint32_t RefCheckChecked(uint32_t r1, uint32_t r3) {
    static const bool check = PortCheck::Enabled("ResCommonRefCheck");
    return Checked("ResCommonRefCheck", check, func_800628A4, RefCheckPort, r1, r3, 0, RefCheckDirect);
}
extern "C" void MH3_ResCommonRefCheck(CpuContext* ctx) {
    ctx->gpr[3] = RefCheckChecked(ctx->gpr[1], ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800628A4, MH3_ResCommonRefCheck, "ResCommonRefCheck");
