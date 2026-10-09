// Character-animation blending, RMHP08. C reconstruction of NW4R g3d's
// AnmObjChrBlend::GetResult (0x800606BC, g3d_anmchr.cpp) from the disassembly.
//
// GetResult(this, result, node) blends the ChrAnmResults of the blend's
// animations (+0x1C objects, +0x20 f32 weights, +0x18 count) that animate the
// node. None: result flags 0. One: that animation's result, unweighted.
// Several, each by its weight over the weights' sum (fres + one Newton step):
//
//   scale        weighted sum, or with flag 0x10 (+0xC) the exp of the
//                weighted sum of the logs; a unit scale (result flag 8) adds
//                the weight, or nothing to the logs
//   rotation     flag 8: slerped quaternions, each into the running blend by
//                its weight over the weights so far; else the weighted sum of
//                the first two rows (identity for result flag 0x20), the third
//                row their cross product, then orthonormalized (falling back to
//                slot 0's rotation when a row degenerates)
//   translation  weighted sum, unless result flag 0x40
//
// and the result flags are the AND of theirs with bit 31 cleared. The
// animations' GetResult (virtual +0x38), logf, expf and the SDK quaternion and
// vector routines stay guest calls; the rest keeps the Gekko single-precision
// arithmetic of the translated code, so the port matches it bit for bit.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>

extern "C" void func_800606BC(CpuContext* ctx);
extern "C" void func_80501404(CpuContext* ctx); // logf
extern "C" void func_80501378(CpuContext* ctx); // expf
extern "C" void func_804C73F0(CpuContext* ctx); // C_QUATMtx
extern "C" void func_804C75C0(CpuContext* ctx); // C_QUATSlerp
extern "C" void func_804C6AB0(CpuContext* ctx); // PSMTXQuat
extern "C" void func_804C72A0(CpuContext* ctx); // PSVECCrossProduct
extern "C" void func_804C71E0(CpuContext* ctx); // PSVECNormalize

namespace {

uint32_t Read32(uint32_t a) { return MemoryInline::FlatRead32(a); }
double Load(uint32_t a) { return MemoryInline::FlatReadFloat32(a); }
void Store(uint32_t a, double v) { MemoryInline::FlatWriteRamFloat32(a, v); }
double Fadds(double a, double b) { return static_cast<double>(PpcForceSingleValueInline(a + b)); }
double Fmuls(double a, double b) { return PpcFmulsInline(a, b); }

// 0x80061110: fres refined by one Newton step, in paired arithmetic.
double Reciprocal(double x) {
    const double estimate = PPC_Fres(PPC_PsFromScalarInline(x));
    const double twice = PPC_PsAddInline(estimate, estimate);
    const double squared = PPC_PsMulInline(estimate, estimate);
    return PPC_PsToScalarInline(PPC_PsNmsubInline(PPC_PsFromScalarInline(x), squared, twice));
}

// 0x8005D5B8: whether the animation animates the node (no 0xC000 bits in its
// u16 node flags).
bool Animates(uint32_t anm, uint32_t node) {
    return (MemoryInline::FlatRead16(Read32(anm + 0x14) + 2 * node) & 0xC000) == 0;
}

// 0x80050F40 (VECSquareMag): (z*z + x*x) + y*y as ps_madd and ps_sum0 do.
double SquareMag(uint32_t v) {
    const double x = Load(v), y = Load(v + 4), z = Load(v + 8);
    const double xy0 = PPC_PsMulInline(PpcPackPairedInline(float(x), float(y)), PpcPackPairedInline(float(x), float(y)));
    const double zz = PPC_PsMaddInline(PPC_PsFromScalarInline(z), PPC_PsFromScalarInline(z), xy0);
    return PPC_PsToScalarInline(PPC_PsSum0Inline(zz, xy0, xy0));
}

constexpr uint32_t kResultBytes = 0x4C;

uint32_t BlendResult(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3], result = ctx->gpr[4], node = ctx->gpr[5];
    const double zero = Load(0x80798208u), one = Load(0x8079820Cu);
    const int32_t count = int32_t(Read32(self + 0x18));
    const uint32_t anms = Read32(self + 0x1C), weights = Read32(self + 0x20);

    double sum = zero;
    uint32_t used = 0, first = 0;
    for (int32_t i = 0; i < count; ++i) {
        const double weight = Load(weights + 4 * i);
        const uint32_t anm = Read32(anms + 4 * i);
        if (anm == 0 || weight == zero || !Animates(anm, node)) continue;
        ++used;
        sum = Fadds(sum, weight);
        if (first == 0) first = anm;
    }
    // The usual cases, none or one animation on the node, need no frame: one
    // is that animation's result, called as the original calls it.
    if (used == 0) {
        MemoryInline::FlatWrite32(result, 0);
        return result;
    }
    if (used == 1) return GuestCall{ctx}.Virtual(first, 0x38, {result, node});

    GuestFrame frame(ctx, 0x170);
    GuestCall game{ctx};
    auto call = [&](void (*fn)(CpuContext*), std::initializer_list<uint32_t> ints,
                    std::initializer_list<double> floats = {}) {
        uint32_t r = 3, f = 1;
        for (uint32_t v : ints) ctx->gpr[r++] = v;
        for (double v : floats) ctx->fpr[f++].d = v;
        fn(ctx);
        return ctx->fpr[1].d;
    };

    const uint32_t mode = Read32(self + 0xC);
    const bool slerp = (mode & 8) != 0, logScale = (mode & 0x10) != 0;
    double logs[3] = {zero, zero, zero};
    const uint32_t blend = frame[0x28], quat = frame[0x18], saved = frame[0x38], own = frame[0x5C];
    Store(blend, zero);
    Store(blend + 4, zero);
    Store(blend + 8, zero);
    Store(blend + 0xC, one);
    double weightsSoFar = zero;
    const double inverseSum = Reciprocal(sum);
    MemoryInline::FlatWrite32(result, 0xFFFFFFFFu);
    for (uint32_t k = 4; k < 0x10; k += 4) Store(result + k, zero);
    const double mtxZero = Load(0x8079F988u);
    for (uint32_t k = 0x1C; k < 0x4C; k += 4) Store(result + k, mtxZero);

    for (int32_t i = 0; i < count; ++i) {
        const uint32_t anm = Read32(anms + 4 * i);
        const double weight = Load(weights + 4 * i);
        if (anm == 0 || weight == zero || !Animates(anm, node)) continue;
        const uint32_t res = game.Virtual<0x38>(anm, {own, node});
        const double w = Fmuls(weight, inverseSum);
        const uint32_t flags = Read32(res);

        const bool unitScale = (flags & 8) != 0;
        if (logScale) {
            if (!unitScale) {
                for (unsigned k = 0; k < 3; ++k) {
                    const double s = Load(res + 4 + 4 * k);
                    const double log = s > zero ? call(func_80501404, {}, {s}) : Load(0x80798210u);
                    logs[k] = Fadds(logs[k], Fmuls(w, log));
                }
            }
        } else {
            for (unsigned k = 0; k < 3; ++k) {
                const uint32_t at = result + 4 + 4 * k;
                Store(at, Fadds(Load(at), unitScale ? w : Fmuls(Load(res + 4 + 4 * k), w)));
            }
        }

        const bool noRotation = (flags & 0x20) != 0;
        if (slerp) {
            if (!noRotation) {
                call(func_804C73F0, {quat, res + 0x1C});
            } else {
                Store(quat, zero);
                Store(quat + 4, zero);
                Store(quat + 8, zero);
                Store(quat + 0xC, one);
            }
            weightsSoFar = Fadds(weightsSoFar, weight);
            const double t = Fmuls(weight, Reciprocal(weightsSoFar));
            call(func_804C75C0, {blend, quat, blend}, {t});
        } else if (!noRotation) {
            if (i == 0) {
                for (unsigned r = 0; r < 3; ++r)
                    for (unsigned c = 0; c < 3; ++c)
                        MemoryInline::FlatWrite32(saved + 12 * r + 4 * c, Read32(res + 0x1C + 16 * r + 4 * c));
            }
            for (uint32_t o : {0x1Cu, 0x20u, 0x24u, 0x2Cu, 0x30u, 0x34u})
                Store(result + o, Fadds(Load(result + o), Fmuls(Load(res + o), w)));
        } else {
            if (i == 0) {
                for (unsigned k = 0; k < 9; ++k) Store(saved + 4 * k, k % 4 == 0 ? Load(0x8079F98Cu) : mtxZero);
            }
            Store(result + 0x1C, Fadds(Load(result + 0x1C), w));
            Store(result + 0x30, Fadds(Load(result + 0x30), w));
        }
        if ((flags & 0x40) == 0) {
            for (uint32_t o : {0x28u, 0x38u, 0x48u}) Store(result + o, Fadds(Load(result + o), Fmuls(Load(res + o), w)));
        }
        MemoryInline::FlatWrite32(result, Read32(result) & flags);
    }

    if (logScale) {
        for (unsigned k = 0; k < 3; ++k) Store(result + 4 + 4 * k, call(func_80501378, {}, {logs[k]}));
    }
    const uint32_t row0 = result + 0x1C, row1 = result + 0x2C, row2 = result + 0x3C;
    if (slerp) {
        uint32_t translation[3];
        for (unsigned k = 0; k < 3; ++k) translation[k] = Read32(result + 0x28 + 0x10 * k);
        call(func_804C6AB0, {row0, blend});
        for (unsigned k = 0; k < 3; ++k) MemoryInline::FlatWrite32(result + 0x28 + 0x10 * k, translation[k]);
    } else {
        call(func_804C72A0, {row0, row1, row2});
        if (SquareMag(row0) == zero || SquareMag(row2) == zero) {
            for (unsigned r = 0; r < 3; ++r)
                for (unsigned c = 0; c < 3; ++c)
                    MemoryInline::FlatWrite32(row0 + 16 * r + 4 * c, Read32(saved + 12 * r + 4 * c));
        } else {
            call(func_804C71E0, {row0, row0});
            call(func_804C71E0, {row2, row2});
            call(func_804C72A0, {row2, row0, row1});
        }
    }
    MemoryInline::FlatWrite32(result, Read32(result) & 0x7FFFFFFFu);
    return result;
}

void BlendPort(CpuContext* ctx) {
    const uint32_t r3 = BlendResult(ctx);
    ctx->gpr[3] = r3;
}

} // namespace

extern "C" void MH3_AnmChrBlendGetResult(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    static const bool check = PortCheck::Enabled("AnmChrBlendGetResult");
    if (check) [[unlikely]] {
        PortCheck::Run(ctx, "AnmChrBlendGetResult", func_800606BC, BlendPort);
        return;
    }
#endif
    BlendPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x800606BC, MH3_AnmChrBlendGetResult, "AnmChrBlendGetResult");
