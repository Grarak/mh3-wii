// Revolution SDK matrix library, RMHP08. C reconstructions of the paired-single
// PSMTXCopy (0x804C62C0), PSMTXConcat (0x804C6300) and PSMTXConcatArray
// (0x804C63D0), from the disassembly.
//
// A Mtx is 3x4 floats, row major. Concat computes ab = a * b with b's implied
// fourth row (0 0 0 1) supplied by a constant pair {0, 1}: PSMTXConcat's at
// 0x807963C8 (multiplied by a's ps1, ps_madds1), PSMTXConcatArray's at
// r13 - 0x4F18 (lane by lane, ps_madd). Both load all of an input before
// writing its product, so the output may alias either input.
//
// The math is the runtime's paired-single lanes (PpcPairVec, the same
// multiply and multiply-add the translated code uses), so the results match it
// bit for bit; the ports save the per-instruction register traffic of the
// translated code and let callers keep a precise call boundary (the
// registered bodies only read r1, r3-r6 and r13).
#include "abi_bridge.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstdint>
#include <cstring>

namespace {

float LoadFloat(uint32_t address) {
    const uint32_t bits = MemoryInline::FlatRead32(address);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
void StoreFloat(uint32_t address, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    MemoryInline::FlatWrite32(address, bits);
}

// psq_l / psq_st with GQR0 (unscaled floats): ps0 at address, ps1 at +4.
// PpcPairVec keeps ps0 in lane1.
PpcPairVec LoadPair(uint32_t address) { return PpcPairVec{LoadFloat(address + 4), LoadFloat(address)}; }
void StorePair(uint32_t address, PpcPairVec v) {
    StoreFloat(address, v.lane1);
    StoreFloat(address + 4, v.lane0);
}
PpcPairVec Ps0(PpcPairVec v) { return PpcPairVec{v.lane1, v.lane1}; }
PpcPairVec Ps1(PpcPairVec v) { return PpcPairVec{v.lane0, v.lane0}; }
PpcPairVec Mul(PpcPairVec a, PpcPairVec c) { return PpcMulPairInline(a, c); }
PpcPairVec Madd(PpcPairVec a, PpcPairVec c, PpcPairVec b) { return PpcFmaddPairInline(a, c, b); }

struct Matrix {
    PpcPairVec p[6]; // +0x00, +0x08, ... +0x28
    explicit Matrix(uint32_t address) {
        for (unsigned i = 0; i < 6; ++i) p[i] = LoadPair(address + 8 * i);
    }
};

void Copy(uint32_t src, uint32_t dst) {
    for (unsigned i = 0; i < 12; ++i) MemoryInline::FlatWrite32(dst + 4 * i, MemoryInline::FlatRead32(src + 4 * i));
}

void Concat(uint32_t a, uint32_t b, uint32_t ab) {
    const Matrix m(a), n(b);
    const PpcPairVec unit = LoadPair(0x807963C8u);
    for (unsigned r = 0; r < 3; ++r) {
        const PpcPairVec x = m.p[2 * r], y = m.p[2 * r + 1];
        const PpcPairVec lo = Madd(n.p[4], Ps0(y), Madd(n.p[2], Ps1(x), Mul(n.p[0], Ps0(x))));
        const PpcPairVec hi = Madd(unit, Ps1(y), Madd(n.p[5], Ps0(y), Madd(n.p[3], Ps1(x), Mul(n.p[1], Ps0(x)))));
        StorePair(ab + 16 * r, lo);
        StorePair(ab + 16 * r + 8, hi);
    }
}

void ConcatArray(uint32_t a, uint32_t srcBase, uint32_t dstBase, uint32_t count, uint32_t r13) {
    const Matrix m(a);
    const PpcPairVec unit = LoadPair(r13 - 0x4F18);
    // The loop counts count - 1 in CTR after the first product: 0 would run
    // it 2^32 times; do one instead.
    for (uint32_t i = 0, n = count ? count : 1; i < n; ++i) {
        const Matrix b(srcBase + 0x30 * i);
        const uint32_t dst = dstBase + 0x30 * i;
        for (unsigned r = 0; r < 3; ++r) {
            const PpcPairVec x = m.p[2 * r], y = m.p[2 * r + 1];
            const PpcPairVec lo = Madd(b.p[4], Ps0(y), Madd(b.p[2], Ps1(x), Mul(b.p[0], Ps0(x))));
            const PpcPairVec hi = Madd(unit, y, Madd(b.p[5], Ps0(y), Madd(b.p[3], Ps1(x), Mul(b.p[1], Ps0(x)))));
            StorePair(dst + 16 * r, lo);
            StorePair(dst + 16 * r + 8, hi);
        }
    }
}

// The packed-double form of the translated code (PPC_Ps* helpers), for the
// ports that mirror it instruction by instruction.
double LoadPacked(uint32_t address) { return PpcPackPairedInline(LoadFloat(address), LoadFloat(address + 4)); }
double LoadPacked1(uint32_t address) { return PpcPackPairedInline(LoadFloat(address), 1.0f); }
void StorePacked(uint32_t address, double v) {
    StoreFloat(address, PpcGetPs0Inline(v));
    StoreFloat(address + 4, PpcGetPs1Inline(v));
}
void StorePacked1(uint32_t address, double v) { StoreFloat(address, PpcGetPs0Inline(v)); }
double Broadcast(uint32_t address) { return PPC_PsFromScalarInline(MemoryInline::FlatReadFloat32(address)); }

// PSMTXMultVec(m, src, dst): dst = m * (src, 1). Per row, ps_mul the row's
// first pair by (x, y), ps_madd its second by (z, 1), ps_sum0 the lanes.
void MultVec(uint32_t m, uint32_t src, uint32_t dst) {
    const double xy = LoadPacked(src), z1 = LoadPacked1(src + 8);
    double out[3];
    for (unsigned r = 0; r < 3; ++r) {
        const double sum = PPC_PsMaddInline(LoadPacked(m + 16 * r + 8), z1, PPC_PsMulInline(LoadPacked(m + 16 * r), xy));
        out[r] = double(float(double(PpcGetPs0Inline(sum)) + PpcGetPs1Inline(sum)));
    }
    for (unsigned r = 0; r < 3; ++r) StoreFloat(dst + 4 * r, float(out[r]));
}

// PSMTXInverse(src, inv): the 3x3 part by cofactors over the determinant
// (fres plus one Newton step), the translation by -inv3x3 * t. Returns 0,
// writing nothing, for a zero determinant.
uint32_t Inverse(uint32_t src, uint32_t inv) {
    const double f0 = LoadPacked1(src), f1 = LoadPacked(src + 4), f2 = LoadPacked1(src + 0x10);
    const double f3 = LoadPacked(src + 0x14), f4 = LoadPacked1(src + 0x20), f5 = LoadPacked(src + 0x24);
    double f6 = PPC_PsMerge10Inline(f1, f0);
    double f7 = PPC_PsMerge10Inline(f3, f2);
    double f8 = PPC_PsMerge10Inline(f5, f4);
    double f11 = PPC_PsMulInline(f3, f6);
    double f13 = PPC_PsMulInline(f5, f7);
    f11 = PPC_PsMsubInline(f1, f7, f11);
    double f12 = PPC_PsMulInline(f1, f8);
    f13 = PPC_PsMsubInline(f3, f8, f13);
    double f10 = PPC_PsMulInline(f3, f4);
    f12 = PPC_PsMsubInline(f5, f6, f12);
    f7 = PPC_PsMulInline(f0, f13);
    double f9 = PPC_PsMulInline(f0, f5);
    f8 = PPC_PsMulInline(f1, f2);
    f7 = PPC_PsMaddInline(f2, f12, f7);
    f6 = PPC_PsSubInline(f6, f6);
    f10 = PPC_PsMsubInline(f2, f5, f10);
    f7 = PPC_PsMaddInline(f4, f11, f7);
    f9 = PPC_PsMsubInline(f1, f4, f9);
    f8 = PPC_PsMsubInline(f0, f3, f8);
    // ps_cmpo0: equal only when ordered and equal.
    if (PpcGetPs0Inline(f7) == PpcGetPs0Inline(f6)) return 0;

    double r0 = PPC_Fres(f7);
    f6 = PPC_PsAddInline(r0, r0);
    double f5b = PPC_PsMulInline(f7, r0);
    r0 = PPC_PsNmsubInline(r0, f5b, f6);
    const double t1 = Broadcast(src + 0xC), t2 = Broadcast(src + 0x1C), t3 = Broadcast(src + 0x2C);
    f13 = PPC_PsMuls0Inline(f13, r0);
    f12 = PPC_PsMuls0Inline(f12, r0);
    f11 = PPC_PsMuls0Inline(f11, r0);
    StorePacked(inv, PPC_PsMerge00Inline(f13, f12));
    StorePacked(inv + 0x10, PPC_PsMerge11Inline(f13, f12));
    double t = PPC_PsMulInline(f13, t1);
    f10 = PPC_PsMuls0Inline(f10, r0);
    f9 = PPC_PsMuls0Inline(f9, r0);
    t = PPC_PsMaddInline(f12, t2, t);
    StorePacked1(inv + 0x20, f10);
    f8 = PPC_PsMuls0Inline(f8, r0);
    t = PPC_PsNmaddInline(f11, t3, t);
    StorePacked1(inv + 0x24, f9);
    double u = PPC_PsMulInline(f10, t1);
    StorePacked1(inv + 0x28, f8);
    u = PPC_PsMaddInline(f9, t2, u);
    StorePacked(inv + 0x08, PPC_PsMerge00Inline(f11, t));
    u = PPC_PsNmaddInline(f8, t3, u);
    StorePacked(inv + 0x18, PPC_PsMerge11Inline(f11, t));
    StorePacked1(inv + 0x2C, u);
    return 1;
}

} // namespace

extern "C" void func_804C62C0(CpuContext* ctx);
extern "C" void func_804C6300(CpuContext* ctx);
extern "C" void func_804C63D0(CpuContext* ctx);
extern "C" void func_804C6560(CpuContext* ctx);
extern "C" void func_804C6F20(CpuContext* ctx);

// With MH3_PORT_CHECK naming the port, each call runs the original too and
// compares the output matrices (registers from the current context).
#ifdef MH3_DEVELOPMENT
#define MH3_MTX_CHECK(name, original, port, out, bytes, ...)                                                  \
    static const bool check = PortCheck::Enabled(name);                                                        \
    if (check) [[unlikely]] {                                                                                  \
        CpuContext* c = TryGetCpuContext();                                                                    \
        __VA_ARGS__;                                                                                           \
        PortCheck::RunOutput(c, name, original, port, {{out, bytes}});                                             \
        return;                                                                                                \
    }
#else
#define MH3_MTX_CHECK(...)
#endif

static void CopyPort(CpuContext* p) { Copy(p->gpr[3], p->gpr[4]); }
static void CopyChecked(uint32_t r1, uint32_t r3, uint32_t r4) {
    MH3_MTX_CHECK("PSMTXCopy", func_804C62C0, CopyPort, r4, 0x30, c->gpr[1] = r1, c->gpr[3] = r3, c->gpr[4] = r4);
    Copy(r3, r4);
}
extern "C" void MH3_PSMTXCopy(CpuContext* ctx) { CopyChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]); }
REGISTER_NATIVE_FUNCTION_AS(0x804C62C0, MH3_PSMTXCopy, "PSMTXCopy");

static void ConcatPort(CpuContext* p) { Concat(p->gpr[3], p->gpr[4], p->gpr[5]); }
static void ConcatChecked(uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5) {
    MH3_MTX_CHECK("PSMTXConcat", func_804C6300, ConcatPort, r5, 0x30, c->gpr[1] = r1, c->gpr[3] = r3,
                  c->gpr[4] = r4, c->gpr[5] = r5);
    Concat(r3, r4, r5);
}
extern "C" void MH3_PSMTXConcat(CpuContext* ctx) { ConcatChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]); }
REGISTER_NATIVE_FUNCTION_AS(0x804C6300, MH3_PSMTXConcat, "PSMTXConcat");

static void ConcatArrayPort(CpuContext* p) { ConcatArray(p->gpr[3], p->gpr[4], p->gpr[5], p->gpr[6], p->gpr[13]); }
static void ConcatArrayChecked(uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r13) {
    MH3_MTX_CHECK("PSMTXConcatArray", func_804C63D0, ConcatArrayPort, r5, 0x30 * (r6 ? r6 : 1), c->gpr[1] = r1,
                  c->gpr[3] = r3, c->gpr[4] = r4, c->gpr[5] = r5, c->gpr[6] = r6, c->gpr[13] = r13);
    ConcatArray(r3, r4, r5, r6, r13);
}
extern "C" void MH3_PSMTXConcatArray(CpuContext* ctx) {
    ConcatArrayChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[13]);
}
REGISTER_NATIVE_FUNCTION_AS(0x804C63D0, MH3_PSMTXConcatArray, "PSMTXConcatArray");

static void MultVecPort(CpuContext* p) { MultVec(p->gpr[3], p->gpr[4], p->gpr[5]); }
static void MultVecChecked(uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5) {
    MH3_MTX_CHECK("PSMTXMultVec", func_804C6F20, MultVecPort, r5, 12, c->gpr[1] = r1, c->gpr[3] = r3, c->gpr[4] = r4,
                  c->gpr[5] = r5);
    MultVec(r3, r4, r5);
}
extern "C" void MH3_PSMTXMultVec(CpuContext* ctx) { MultVecChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]); }
REGISTER_NATIVE_FUNCTION_AS(0x804C6F20, MH3_PSMTXMultVec, "PSMTXMultVec");

static void InversePort(CpuContext* p) { p->gpr[3] = Inverse(p->gpr[3], p->gpr[4]); }
static uint32_t InverseChecked(uint32_t r1, uint32_t r3, uint32_t r4) {
#ifdef MH3_DEVELOPMENT
    static const bool check = PortCheck::Enabled("PSMTXInverse");
    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        PortCheck::RunOutput(c, "PSMTXInverse", func_804C6560, InversePort, {{r4, 0x30}});
        return c->gpr[3];
    }
#else
    (void)r1;
#endif
    return Inverse(r3, r4);
}
extern "C" void MH3_PSMTXInverse(CpuContext* ctx) { ctx->gpr[3] = InverseChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]); }
REGISTER_NATIVE_FUNCTION_AS(0x804C6560, MH3_PSMTXInverse, "PSMTXInverse");
