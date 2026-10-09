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
#include "g3d_native.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cmath>
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
PpcPairVec LoadPair(uint32_t address) {
    return PpcPairVec{LoadFloat(address + 4), LoadFloat(address)};
}
void StorePair(uint32_t address, PpcPairVec v) {
    StoreFloat(address, PpcPairLaneInline(v, 1));
    StoreFloat(address + 4, PpcPairLaneInline(v, 0));
}
PpcPairVec Ps0(PpcPairVec v) {
    const float ps0 = PpcPairLaneInline(v, 1);
    return PpcPairVec{ps0, ps0};
}
PpcPairVec Ps1(PpcPairVec v) {
    const float ps1 = PpcPairLaneInline(v, 0);
    return PpcPairVec{ps1, ps1};
}
PpcPairVec Mul(PpcPairVec a, PpcPairVec c) {
    return PpcMulPairInline(a, c);
}
PpcPairVec Madd(PpcPairVec a, PpcPairVec c, PpcPairVec b) {
    return PpcFmaddPairInline(a, c, b);
}

struct Matrix {
    PpcPairVec p[6]; // +0x00, +0x08, ... +0x28
    Matrix() = default;
    explicit Matrix(uint32_t address) {
        for (unsigned i = 0; i < 6; ++i)
            p[i] = LoadPair(address + 8 * i);
    }
    // Element (row, column): ps0 of a pair is lane1.
    float Get(unsigned row, unsigned column) const { return PpcPairLaneInline(p[2 * row + column / 2], column % 2 == 0 ? 1 : 0); }
    void Set(unsigned row, unsigned column, float value) { PpcSetPairLaneInline(p[2 * row + column / 2], column % 2 == 0 ? 1 : 0, value); }
    void Store(uint32_t address) const {
        for (unsigned i = 0; i < 6; ++i)
            StorePair(address + 8 * i, p[i]);
    }
};

// PSMTXConcat's product of host matrices.
Matrix ConcatMatrices(const Matrix& m, const Matrix& n) {
    const PpcPairVec unit = LoadPair(0x807963C8u);
    Matrix ab;
    for (unsigned r = 0; r < 3; ++r) {
        const PpcPairVec x = m.p[2 * r], y = m.p[2 * r + 1];
        ab.p[2 * r] = Madd(n.p[4], Ps0(y), Madd(n.p[2], Ps1(x), Mul(n.p[0], Ps0(x))));
        ab.p[2 * r + 1] = Madd(unit, Ps1(y), Madd(n.p[5], Ps0(y), Madd(n.p[3], Ps1(x), Mul(n.p[1], Ps0(x)))));
    }
    return ab;
}

void Copy(uint32_t src, uint32_t dst) {
    for (unsigned i = 0; i < 12; ++i)
        MemoryInline::FlatWrite32(dst + 4 * i, MemoryInline::FlatRead32(src + 4 * i));
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
double LoadPacked(uint32_t address) {
    return PpcPackPairedInline(LoadFloat(address), LoadFloat(address + 4));
}
double LoadPacked1(uint32_t address) {
    return PpcPackPairedInline(LoadFloat(address), 1.0f);
}
void StorePacked(uint32_t address, double v) {
    StoreFloat(address, PpcGetPs0Inline(v));
    StoreFloat(address + 4, PpcGetPs1Inline(v));
}
void StorePacked1(uint32_t address, double v) {
    StoreFloat(address, PpcGetPs0Inline(v));
}
double Broadcast(uint32_t address) {
    return PPC_PsFromScalarInline(MemoryInline::FlatReadFloat32(address));
}

// PSMTXMultVec(m, src, dst): dst = m * (src, 1). Per row, ps_mul the row's
// first pair by (x, y), ps_madd its second by (z, 1), ps_sum0 the lanes.
void MultVec(uint32_t m, uint32_t src, uint32_t dst) {
    const double xy = LoadPacked(src), z1 = LoadPacked1(src + 8);
    double out[3];
    for (unsigned r = 0; r < 3; ++r) {
        const double sum = PPC_PsMaddInline(LoadPacked(m + 16 * r + 8), z1, PPC_PsMulInline(LoadPacked(m + 16 * r), xy));
        out[r] = double(float(double(PpcGetPs0Inline(sum)) + PpcGetPs1Inline(sum)));
    }
    for (unsigned r = 0; r < 3; ++r)
        StoreFloat(dst + 4 * r, float(out[r]));
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
    if (PpcGetPs0Inline(f7) == PpcGetPs0Inline(f6))
        return 0;

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
#define MH3_MTX_CHECK(name, original, port, out, bytes, ...)                                                                                                                                           \
    static const bool check = PortCheck::Enabled(name);                                                                                                                                                \
    if (check) [[unlikely]] {                                                                                                                                                                          \
        CpuContext* c = TryGetCpuContext();                                                                                                                                                            \
        __VA_ARGS__;                                                                                                                                                                                   \
        PortCheck::RunOutput(c, name, original, port, {{out, bytes}});                                                                                                                                 \
        return;                                                                                                                                                                                        \
    }
#else
#define MH3_MTX_CHECK(...)
#endif

static void CopyPort(CpuContext* p) {
    Copy(p->gpr[3], p->gpr[4]);
}
static void CopyChecked(uint32_t r1, uint32_t r3, uint32_t r4) {
    MH3_MTX_CHECK("PSMTXCopy", func_804C62C0, CopyPort, r4, 0x30, c->gpr[1] = r1, c->gpr[3] = r3, c->gpr[4] = r4);
    Copy(r3, r4);
}
extern "C" void MH3_PSMTXCopy(CpuContext* ctx) {
    CopyChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x804C62C0, MH3_PSMTXCopy, "PSMTXCopy");

static void ConcatPort(CpuContext* p) {
    Concat(p->gpr[3], p->gpr[4], p->gpr[5]);
}
static void ConcatChecked(uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5) {
    MH3_MTX_CHECK("PSMTXConcat", func_804C6300, ConcatPort, r5, 0x30, c->gpr[1] = r1, c->gpr[3] = r3, c->gpr[4] = r4, c->gpr[5] = r5);
    Concat(r3, r4, r5);
}
extern "C" void MH3_PSMTXConcat(CpuContext* ctx) {
    ConcatChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x804C6300, MH3_PSMTXConcat, "PSMTXConcat");

static void ConcatArrayPort(CpuContext* p) {
    ConcatArray(p->gpr[3], p->gpr[4], p->gpr[5], p->gpr[6], p->gpr[13]);
}
static void ConcatArrayChecked(uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r13) {
    MH3_MTX_CHECK("PSMTXConcatArray", func_804C63D0, ConcatArrayPort, r5, 0x30 * (r6 ? r6 : 1), c->gpr[1] = r1, c->gpr[3] = r3, c->gpr[4] = r4, c->gpr[5] = r5, c->gpr[6] = r6, c->gpr[13] = r13);
    ConcatArray(r3, r4, r5, r6, r13);
}
extern "C" void MH3_PSMTXConcatArray(CpuContext* ctx) {
    ConcatArrayChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[13]);
}
REGISTER_NATIVE_FUNCTION_AS(0x804C63D0, MH3_PSMTXConcatArray, "PSMTXConcatArray");

static void MultVecPort(CpuContext* p) {
    MultVec(p->gpr[3], p->gpr[4], p->gpr[5]);
}
static void MultVecChecked(uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5) {
    MH3_MTX_CHECK("PSMTXMultVec", func_804C6F20, MultVecPort, r5, 12, c->gpr[1] = r1, c->gpr[3] = r3, c->gpr[4] = r4, c->gpr[5] = r5);
    MultVec(r3, r4, r5);
}
extern "C" void MH3_PSMTXMultVec(CpuContext* ctx) {
    MultVecChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x804C6F20, MH3_PSMTXMultVec, "PSMTXMultVec");

static void InversePort(CpuContext* p) {
    p->gpr[3] = Inverse(p->gpr[3], p->gpr[4]);
}
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
extern "C" void MH3_PSMTXInverse(CpuContext* ctx) {
    ctx->gpr[3] = InverseChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x804C6560, MH3_PSMTXInverse, "PSMTXInverse");

// 0x80501AB4 (MTX34RotXYZFIdx, nw4r::math): m = Rz * Ry * Rx from three
// angles in FIdx units (a turn is the float at r2 - 0x5C8, 256). Each |angle|
// is wrapped to [0, 256] by subtracting a turn while it is above one, split by
// a GQR3 (u16, the SDK's fast-cast setting) quantized store into a table index
// (its low byte) and the fraction, and looked up in the {sin, cos, dsin, dcos}
// table at 0x805744A0 with linear interpolation; a negative angle negates its
// sine. The multiplies and multiply-adds are the paired-single lanes'.
namespace {

struct SinCos {
    float s, c;
};

SinCos SinCosFIdx(float angle, float turn) {
    float a = std::fabs(angle);
    while (a > turn)
        a = a - turn;
    const float whole = float(uint16_t(a)); // psq_st u16 (truncating), psq_l back
    const float fraction = a - whole;
    const uint32_t entry = 0x805744A0u + ((uint32_t(uint16_t(a)) & 0xFF) << 4);
    SinCos r{PpcFmaFloatInline(LoadFloat(entry + 8), fraction, LoadFloat(entry)), PpcFmaFloatInline(LoadFloat(entry + 12), fraction, LoadFloat(entry + 4))};
    if (angle < 0.0f)
        r.s = -r.s;
    return r;
}

void RotXYZFIdx(uint32_t m, float x, float y, float z, uint32_t r2) {
    const float turn = LoadFloat(r2 - 0x5C8);
    const SinCos rx = SinCosFIdx(x, turn), ry = SinCosFIdx(y, turn), rz = SinCosFIdx(z, turn);
    const float sx = rx.s, cx = rx.c, sy = ry.s, cy = ry.c, sz = rz.s, cz = rz.c;
    StoreFloat(m + 0x2C, sx - sx); // ps_sub f7,f0,f0: the translation's 0
    const float zero = sx - sx;
    StoreFloat(m + 0x00, cz * cy);
    StoreFloat(m + 0x04, PpcFmaFloatInline(sx * cz, sy, -cx * sz));
    StoreFloat(m + 0x08, PpcFmaFloatInline(cx * cz, sy, sx * sz));
    StoreFloat(m + 0x0C, zero);
    StoreFloat(m + 0x10, sz * cy);
    StoreFloat(m + 0x14, PpcFmaFloatInline(sx * sz, sy, -(-cx * cz)));
    StoreFloat(m + 0x18, PpcFmaFloatInline(cx * sz, sy, -(sx * cz)));
    StoreFloat(m + 0x1C, zero);
    StoreFloat(m + 0x20, -sy);
    StoreFloat(m + 0x24, sx * cy);
    StoreFloat(m + 0x28, cx * cy);
}

} // namespace

void G3dNative::MtxRotXYZFIdx(uint32_t m, float x, float y, float z, uint32_t r2) {
    RotXYZFIdx(m, x, y, z, r2);
}

extern "C" void func_80501AB4(CpuContext* ctx);
static void RotXYZFIdxPort(CpuContext* p) {
    RotXYZFIdx(p->gpr[3], float(p->fpr[1].d), float(p->fpr[2].d), float(p->fpr[3].d), p->gpr[2]);
}
static void RotXYZFIdxChecked(uint32_t r1, uint32_t r2, uint32_t m, double x, double y, double z) {
    MH3_MTX_CHECK("MTX34RotXYZFIdx", func_80501AB4, RotXYZFIdxPort, m, 0x30, c->gpr[1] = r1, c->gpr[2] = r2, c->gpr[3] = m, c->fpr[1].d = x, c->fpr[2].d = y, c->fpr[3].d = z);
    RotXYZFIdx(m, float(x), float(y), float(z), r2);
}
extern "C" void MH3_MTX34RotXYZFIdx(CpuContext* ctx) {
    RotXYZFIdxChecked(ctx->gpr[1], ctx->gpr[2], ctx->gpr[3], ctx->fpr[1].d, ctx->fpr[2].d, ctx->fpr[3].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x80501AB4, MH3_MTX34RotXYZFIdx, "MTX34RotXYZFIdx");

// 0x80501A10 (MTX34MultTrans, nw4r::math): out = m * Trans(v): m's rotation,
// and per row the translation m[r][0..2] . v + m[r][3], summed as the paired
// code does: (m2 * vz + m0 * vx) + (m3 * 1 + m1 * vy).
namespace {

Matrix MultTrans(const Matrix& m, float vx, float vy, float vz) {
    const PpcPairVec xy{vy, vx}, z1{1.0f, vz};
    Matrix out = m;
    for (unsigned r = 0; r < 3; ++r) {
        const PpcPairVec sum = Madd(m.p[2 * r + 1], z1, Mul(m.p[2 * r], xy));
        out.Set(r, 3, PpcPairLaneInline(sum, 1) + PpcPairLaneInline(sum, 0));
    }
    return out;
}

// 0x800D7814 (WorldMtxNodeStepMayaScale): a node's world matrix and scale from
// its parent's with Maya segment-scale compensation. node: u32 flags, VEC3
// scale at +4, the local Mtx34 at +0x1C. flags (r7) carry the calc state; bit
// 31 says the accumulated scale is 1. Node flag 2 or 4: world = parent;
// 0x20: translation only (world = parent * Trans(t), t scaled unless bit 31);
// else world = parent * local (its translation scaled unless bit 31).
// Then the world scale: node flag 8 keeps the parent's (bit 30 set), else
// parent scale * node scale (bit 30 cleared with 31); node flag 0x10 sets bit
// 28, else bits 28-31 are cleared. Returns the flags.
uint32_t WorldMtxNodeStepMayaScale(uint32_t world, uint32_t worldScale, uint32_t parent, uint32_t scale, uint32_t flags, uint32_t node) {
    const uint32_t nodeFlags = MemoryInline::FlatRead32(node);
    const bool unitScale = (flags & 0x80000000u) != 0;
    const float sx = LoadFloat(scale), sy = LoadFloat(scale + 4), sz = LoadFloat(scale + 8);
    if ((nodeFlags & 6) != 0) {
        Copy(parent, world);
    } else if ((nodeFlags & 0x20) != 0) {
        const float tx = LoadFloat(node + 0x28), ty = LoadFloat(node + 0x38), tz = LoadFloat(node + 0x48);
        const Matrix m(parent);
        (unitScale ? MultTrans(m, tx, ty, tz) : MultTrans(m, sx * tx, sy * ty, sz * tz)).Store(world);
    } else {
        Matrix local(node + 0x1C);
        if (!unitScale) {
            local.Set(0, 3, local.Get(0, 3) * sx);
            local.Set(1, 3, local.Get(1, 3) * sy);
            local.Set(2, 3, local.Get(2, 3) * sz);
        }
        ConcatMatrices(Matrix(parent), local).Store(world);
    }
    if ((nodeFlags & 8) != 0) {
        flags |= 0x40000000u;
        StoreFloat(worldScale, sx);
        StoreFloat(worldScale + 4, sy);
        StoreFloat(worldScale + 8, sz);
    } else {
        flags &= 0x3FFFFFFFu;
        StoreFloat(worldScale, sx * LoadFloat(node + 4));
        StoreFloat(worldScale + 4, sy * LoadFloat(node + 8));
        StoreFloat(worldScale + 8, sz * LoadFloat(node + 0xC));
    }
    return (nodeFlags & 0x10) != 0 ? flags | 0x10000000u : flags & 0x0FFFFFFFu;
}

} // namespace

extern "C" void func_80501A10(CpuContext* ctx);
static void MultTransPort(CpuContext* p) {
    MultTrans(Matrix(p->gpr[4]), LoadFloat(p->gpr[5]), LoadFloat(p->gpr[5] + 4), LoadFloat(p->gpr[5] + 8)).Store(p->gpr[3]);
}
static void MultTransChecked(uint32_t r1, uint32_t out, uint32_t m, uint32_t v) {
    MH3_MTX_CHECK("MTX34MultTrans", func_80501A10, MultTransPort, out, 0x30, c->gpr[1] = r1, c->gpr[3] = out, c->gpr[4] = m, c->gpr[5] = v);
    MultTrans(Matrix(m), LoadFloat(v), LoadFloat(v + 4), LoadFloat(v + 8)).Store(out);
}
extern "C" void MH3_MTX34MultTrans(CpuContext* ctx) {
    MultTransChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80501A10, MH3_MTX34MultTrans, "MTX34MultTrans");

extern "C" void func_800D7814(CpuContext* ctx);
static void WorldStepPort(CpuContext* p) {
    p->gpr[3] = WorldMtxNodeStepMayaScale(p->gpr[3], p->gpr[4], p->gpr[5], p->gpr[6], p->gpr[7], p->gpr[8]);
}
static uint32_t WorldStepChecked(uint32_t r1, uint32_t world, uint32_t worldScale, uint32_t parent, uint32_t scale, uint32_t flags, uint32_t node) {
#ifdef MH3_DEVELOPMENT
    static const bool check = PortCheck::Enabled("WorldMtxNodeStepMayaScale");
    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = world;
        c->gpr[4] = worldScale;
        c->gpr[5] = parent;
        c->gpr[6] = scale;
        c->gpr[7] = flags;
        c->gpr[8] = node;
        PortCheck::RunOutput(c, "WorldMtxNodeStepMayaScale", func_800D7814, WorldStepPort, {{world, 0x30}, {worldScale, 12}});
        return c->gpr[3];
    }
#else
    (void)r1;
#endif
    return WorldMtxNodeStepMayaScale(world, worldScale, parent, scale, flags, node);
}
extern "C" void MH3_WorldMtxNodeStepMayaScale(CpuContext* ctx) {
    ctx->gpr[3] = WorldStepChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->gpr[8]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800D7814, MH3_WorldMtxNodeStepMayaScale, "WorldMtxNodeStepMayaScale");

// 0x805019BC (MTX34Scale, nw4r::math): out = m * Scale(s), each row's
// (x, y) pair by (sx, sy) and its (z, t) pair by (sz, 1).
namespace {

void ApplyScale(uint32_t out, uint32_t m, uint32_t s) {
    const PpcPairVec xy = LoadPair(s), z1{1.0f, LoadFloat(s + 8)};
    const Matrix a(m);
    for (unsigned r = 0; r < 3; ++r) {
        StorePair(out + 16 * r, Mul(a.p[2 * r], xy));
        StorePair(out + 16 * r + 8, Mul(a.p[2 * r + 1], z1));
    }
}

} // namespace

extern "C" void func_805019BC(CpuContext* ctx);
static void ApplyScalePort(CpuContext* p) {
    ApplyScale(p->gpr[3], p->gpr[4], p->gpr[5]);
}
static void ApplyScaleChecked(uint32_t r1, uint32_t out, uint32_t m, uint32_t s) {
    MH3_MTX_CHECK("MTX34Scale", func_805019BC, ApplyScalePort, out, 0x30, c->gpr[1] = r1, c->gpr[3] = out, c->gpr[4] = m, c->gpr[5] = s);
    ApplyScale(out, m, s);
}
extern "C" void MH3_MTX34Scale(CpuContext* ctx) {
    ApplyScaleChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x805019BC, MH3_MTX34Scale, "MTX34Scale");

void G3dNative::MtxCopy(uint32_t src, uint32_t dst) {
    Copy(src, dst);
}
void G3dNative::MtxMultVec(uint32_t m, uint32_t src, uint32_t dst) {
    MultVec(m, src, dst);
}
void G3dNative::MtxConcat(uint32_t a, uint32_t b, uint32_t ab) {
    Concat(a, b, ab);
}
void G3dNative::MtxApplyScale(uint32_t out, uint32_t m, uint32_t s) {
    ApplyScale(out, m, s);
}
uint32_t G3dNative::WorldMtxNodeStepMayaScale(uint32_t world, uint32_t worldScale, uint32_t parent, uint32_t scale, uint32_t flags, uint32_t node) {
    return ::WorldMtxNodeStepMayaScale(world, worldScale, parent, scale, flags, node);
}

// nw4r::math's MTX34Inv (0x80331064), MTX34InvTranspose to a Mtx33
// (0x80330E78) and to a Mtx34 (0x80330F5C), and MTX34ToMTX33 (0x80501968).
// The three inverses share PSMTXInverse's cofactors but call a matrix
// singular when |det| is below the epsilon pair at 0x8079D6F0 (ps_cmpo0: a
// NaN determinant is not below it). Mirrored instruction by instruction.
namespace {

struct Cofactors {
    double f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13;
    bool singular;
};

Cofactors LoadCofactors(uint32_t src) {
    Cofactors c;
    c.f0 = LoadPacked1(src);
    c.f1 = LoadPacked(src + 4);
    c.f2 = LoadPacked1(src + 0x10);
    c.f6 = PPC_PsMerge10Inline(c.f1, c.f0);
    c.f3 = LoadPacked(src + 0x14);
    c.f4 = LoadPacked1(src + 0x20);
    c.f7 = PPC_PsMerge10Inline(c.f3, c.f2);
    c.f5 = LoadPacked(src + 0x24);
    c.f11 = PPC_PsMulInline(c.f3, c.f6);
    c.f8 = PPC_PsMerge10Inline(c.f5, c.f4);
    c.f13 = PPC_PsMulInline(c.f5, c.f7);
    c.f11 = PPC_PsMsubInline(c.f1, c.f7, c.f11);
    c.f12 = PPC_PsMulInline(c.f1, c.f8);
    c.f13 = PPC_PsMsubInline(c.f3, c.f8, c.f13);
    c.f12 = PPC_PsMsubInline(c.f5, c.f6, c.f12);
    c.f7 = PPC_PsMulInline(c.f0, c.f13);
    c.f10 = PPC_PsMulInline(c.f3, c.f4);
    c.f7 = PPC_PsMaddInline(c.f2, c.f12, c.f7);
    c.f6 = LoadPacked1(0x8079D6F0u);
    c.f9 = PPC_PsMulInline(c.f0, c.f5);
    c.f7 = PPC_PsMaddInline(c.f4, c.f11, c.f7);
    c.f10 = PPC_PsMsubInline(c.f2, c.f5, c.f10);
    c.f9 = PPC_PsMsubInline(c.f1, c.f4, c.f9);
    c.f8 = PPC_PsAbsInline(c.f7);
    c.singular = PpcGetPs0Inline(c.f8) < PpcGetPs0Inline(c.f6);
    return c;
}

// fres of the determinant refined by one Newton step, and the last cofactor.
double Reciprocal(Cofactors& c) {
    const double r = PPC_Fres(c.f7);
    c.f8 = PPC_PsMulInline(c.f1, c.f2);
    const double twice = PPC_PsAddInline(r, r);
    c.f8 = PPC_PsMsubInline(c.f0, c.f3, c.f8);
    const double rd = PPC_PsMulInline(c.f7, r);
    return PPC_PsNmsubInline(r, rd, twice);
}

uint32_t Inv34(uint32_t dst, uint32_t src) {
    Cofactors c = LoadCofactors(src);
    if (c.singular)
        return 0;
    const double r = Reciprocal(c);
    const double t1 = Broadcast(src + 0xC), t2 = Broadcast(src + 0x1C), t3 = Broadcast(src + 0x2C);
    const double f13 = PPC_PsMuls0Inline(c.f13, r), f12 = PPC_PsMuls0Inline(c.f12, r);
    const double f11 = PPC_PsMuls0Inline(c.f11, r);
    StorePacked(dst, PPC_PsMerge00Inline(f13, f12));
    StorePacked(dst + 0x10, PPC_PsMerge11Inline(f13, f12));
    double t = PPC_PsMulInline(f13, t1);
    const double f10 = PPC_PsMuls0Inline(c.f10, r), f9 = PPC_PsMuls0Inline(c.f9, r);
    t = PPC_PsMaddInline(f12, t2, t);
    StorePacked1(dst + 0x20, f10);
    const double f8 = PPC_PsMuls0Inline(c.f8, r);
    t = PPC_PsNmaddInline(f11, t3, t);
    StorePacked1(dst + 0x24, f9);
    double u = PPC_PsMulInline(f10, t1);
    StorePacked1(dst + 0x28, f8);
    u = PPC_PsMaddInline(f9, t2, u);
    StorePacked(dst + 0x08, PPC_PsMerge00Inline(f11, t));
    u = PPC_PsNmaddInline(f8, t3, u);
    StorePacked(dst + 0x18, PPC_PsMerge11Inline(f11, t));
    StorePacked1(dst + 0x2C, u);
    return 1;
}

uint32_t InvTranspose33(uint32_t dst, uint32_t src) {
    Cofactors c = LoadCofactors(src);
    if (c.singular) {
        StorePacked1(dst, c.f0);
        StorePacked1(dst + 0xC, c.f2);
        StorePacked1(dst + 0x18, c.f4);
        StorePacked(dst + 4, c.f1);
        StorePacked(dst + 0x10, c.f3);
        StorePacked(dst + 0x1C, c.f5);
        return 0;
    }
    const double r = Reciprocal(c);
    StorePacked(dst, PPC_PsMuls0Inline(c.f13, r));
    StorePacked(dst + 0xC, PPC_PsMuls0Inline(c.f12, r));
    StorePacked(dst + 0x18, PPC_PsMuls0Inline(c.f11, r));
    StorePacked1(dst + 0x8, PPC_PsMuls0Inline(c.f10, r));
    StorePacked1(dst + 0x14, PPC_PsMuls0Inline(c.f9, r));
    StorePacked1(dst + 0x20, PPC_PsMuls0Inline(c.f8, r));
    return 1;
}

uint32_t InvTranspose34(uint32_t dst, uint32_t src) {
    Cofactors c = LoadCofactors(src);
    const double zero = PPC_PsSubInline(c.f6, c.f6);
    if (c.singular) {
        if (dst == src)
            return 0;
        StorePacked1(dst, c.f0);
        StorePacked1(dst + 0x10, c.f2);
        StorePacked1(dst + 0x20, c.f4);
        StorePacked(dst + 4, c.f1);
        StorePacked(dst + 0x14, c.f3);
        StorePacked(dst + 0x24, c.f5);
        StorePacked1(dst + 0xC, zero);
        StorePacked1(dst + 0x1C, zero);
        StorePacked1(dst + 0x2C, zero);
        return 0;
    }
    const double r = Reciprocal(c);
    StorePacked1(dst + 0xC, zero);
    StorePacked1(dst + 0x1C, zero);
    StorePacked1(dst + 0x2C, zero);
    StorePacked(dst, PPC_PsMuls0Inline(c.f13, r));
    StorePacked(dst + 0x10, PPC_PsMuls0Inline(c.f12, r));
    StorePacked(dst + 0x20, PPC_PsMuls0Inline(c.f11, r));
    StorePacked1(dst + 0x8, PPC_PsMuls0Inline(c.f10, r));
    StorePacked1(dst + 0x18, PPC_PsMuls0Inline(c.f9, r));
    StorePacked1(dst + 0x28, PPC_PsMuls0Inline(c.f8, r));
    return 1;
}

// The rotation part of a Mtx34 as a Mtx33 (float bits copied through
// psq_l/psq_st, which is exact for floats).
void To33(uint32_t dst, uint32_t src) {
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned k = 0; k < 3; ++k)
            MemoryInline::FlatWrite32(dst + 12 * r + 4 * k, MemoryInline::FlatRead32(src + 16 * r + 4 * k));
}

} // namespace

extern "C" void func_80331064(CpuContext* ctx);
extern "C" void func_80330E78(CpuContext* ctx);
extern "C" void func_80330F5C(CpuContext* ctx);
extern "C" void func_80501968(CpuContext* ctx);

#ifdef MH3_DEVELOPMENT
#define MH3_MTX_RESULT_CHECK(name, original, port, out, bytes)                                                                                                                                         \
    static const bool check = PortCheck::Enabled(name);                                                                                                                                                \
    if (check) [[unlikely]] {                                                                                                                                                                          \
        PortCheck::RunOutput(ctx, name, original, port, {{out, bytes}});                                                                                                                               \
        return;                                                                                                                                                                                        \
    }
#else
#define MH3_MTX_RESULT_CHECK(...)
#endif

static void Inv34Port(CpuContext* p) {
    p->gpr[3] = Inv34(p->gpr[3], p->gpr[4]);
}
extern "C" void MH3_MTX34Inv(CpuContext* ctx) {
    MH3_MTX_RESULT_CHECK("MTX34Inv", func_80331064, Inv34Port, ctx->gpr[3], 0x30);
    Inv34Port(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80331064, MH3_MTX34Inv, "MTX34Inv");

static void InvTranspose33Port(CpuContext* p) {
    p->gpr[3] = InvTranspose33(p->gpr[3], p->gpr[4]);
}
extern "C" void MH3_MTX34InvTransposeTo33(CpuContext* ctx) {
    MH3_MTX_RESULT_CHECK("MTX34InvTransposeTo33", func_80330E78, InvTranspose33Port, ctx->gpr[3], 0x24);
    InvTranspose33Port(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80330E78, MH3_MTX34InvTransposeTo33, "MTX34InvTransposeTo33");

static void InvTranspose34Port(CpuContext* p) {
    p->gpr[3] = InvTranspose34(p->gpr[3], p->gpr[4]);
}
extern "C" void MH3_MTX34InvTranspose(CpuContext* ctx) {
    MH3_MTX_RESULT_CHECK("MTX34InvTranspose", func_80330F5C, InvTranspose34Port, ctx->gpr[3], 0x30);
    InvTranspose34Port(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80330F5C, MH3_MTX34InvTranspose, "MTX34InvTranspose");

static void To33Port(CpuContext* p) {
    To33(p->gpr[3], p->gpr[4]);
}
extern "C" void MH3_MTX34ToMTX33(CpuContext* ctx) {
    MH3_MTX_RESULT_CHECK("MTX34ToMTX33", func_80501968, To33Port, ctx->gpr[3], 0x24);
    To33Port(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80501968, MH3_MTX34ToMTX33, "MTX34ToMTX33");

uint32_t G3dNative::MtxInv34(uint32_t dst, uint32_t src) {
    return Inv34(dst, src);
}
uint32_t G3dNative::MtxInvTranspose33(uint32_t dst, uint32_t src) {
    return InvTranspose33(dst, src);
}
uint32_t G3dNative::MtxInvTranspose34(uint32_t dst, uint32_t src) {
    return InvTranspose34(dst, src);
}
void G3dNative::MtxTo33(uint32_t dst, uint32_t src) {
    To33(dst, src);
}
void G3dNative::MtxConcatArray(uint32_t a, uint32_t src, uint32_t dst, uint32_t count, uint32_t r13) {
    ConcatArray(a, src, dst, count, r13);
}
