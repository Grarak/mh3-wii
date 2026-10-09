// ef_util.cpp's matrix scale (0x8009C0A4, EfMtxGetScale(m, VEC3* out)),
// RMHP08. C reconstruction from the disassembly. The particle code's
// inherited-matrix path (0x800A9110) and fields take a matrix's scale this
// way: Gram-Schmidt over its columns. Each column whose squared length is
// above a small bound (r2 - 31508) is normalized with FrSqrt, its length
// FastReciprocal of that, and the later columns lose their parts along it;
// a column at or below the bound has scale 0. The last column's length is
// FSqrt (or VecMag when the second column vanished), and a left-handed basis
// (col0 . (col1 x col2) < 0) negates all three. The vector helpers are the
// paired-single ones of the translation (PSVECDotProduct 0x80052278, the
// squared length 0x80050F40, VecScale3 0x80051488, VecSubtract3 0x80050D58,
// PSVECCrossProduct 0x804C72A0).
#include "abi_bridge.h"
#include "memory.h"
#include "nw4r_math.h"
#include "port_check.h"
#include "ppc_runtime.h"
#include "ps_vec_math.h"

#include <cstdint>

namespace {

struct Vec {
    float x, y, z;
};

constexpr uint32_t kBound = 0x8079844Cu; // r2 - 31508
constexpr uint32_t kZero = 0x8079843Cu;  // r2 - 31524

float ReadFloat(uint32_t a) {
    return MemoryInline::FlatReadFloat32(a);
}

// 0x8009BADC: column i of the matrix.
Vec Column(uint32_t m, uint32_t i) {
    return {ReadFloat(m + 4 * i), ReadFloat(m + 0x10 + 4 * i), ReadFloat(m + 0x20 + 4 * i)};
}

double MagSq(const Vec& v) {
    const double xy = PPC_PsMulInline(PpcPackPairedInline(v.x, v.y), PpcPackPairedInline(v.x, v.y));
    const double z = PPC_PsFromScalarInline(v.z);
    return PPC_PsToScalarInline(PPC_PsSum0Inline(PPC_PsMaddInline(z, z, xy), xy, xy));
}

float Dot(const Vec& a, const Vec& b) {
    const double yz = PPC_PsMulInline(PpcPackPairedInline(a.y, a.z), PpcPackPairedInline(b.y, b.z));
    const double x = PPC_PsMaddInline(PpcPackPairedInline(a.x, 1.0f), PpcPackPairedInline(b.x, 1.0f), yz);
    return float(PPC_PsToScalarInline(PPC_PsSum0Inline(x, yz, yz)));
}

Vec Scale(const Vec& v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

Vec Sub(const Vec& a, const Vec& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec Cross(const Vec& a, const Vec& b) {
    const double ab = PpcPackPairedInline(a.x, a.y), bb = PpcPackPairedInline(b.x, b.y);
    const double az = PPC_PsFromScalarInline(a.z), bz = PPC_PsFromScalarInline(b.z);
    const double f4 = PPC_PsMulInline(bb, az);
    const double f7 = PPC_PsMuls0Inline(bb, ab);
    const double f5 = PPC_PsMsubInline(ab, bz, f4);
    const double f8 = PPC_PsMsubInline(ab, PPC_PsMerge10Inline(bb, bb), f7);
    const double f10 = PPC_PsNegInline(PPC_PsMerge01Inline(f5, f8));
    return {float(PPC_PsToScalarInline(PPC_PsMerge11Inline(f5, f5))), float(PPC_PsToScalarInline(f10)), PpcGetPs1Inline(f10)};
}

void GetScale(uint32_t m, uint32_t out) {
    const float bound = ReadFloat(kBound), zero = ReadFloat(kZero);
    float s[3] = {zero, zero, zero};
    Vec c0 = Column(m, 0);

    if (const double sq0 = MagSq(c0); sq0 > bound) {
        const float e0 = Nw4rMath::FrSqrt(sq0);
        s[0] = Nw4rMath::FastReciprocal(e0);
        c0 = Scale(c0, e0);
        Vec c1 = Column(m, 1);
        c1 = Sub(c1, Scale(c0, Dot(c0, c1)));

        if (const double sq1 = MagSq(c1); sq1 > bound) {
            const float e1 = Nw4rMath::FrSqrt(sq1);
            s[1] = Nw4rMath::FastReciprocal(e1);
            c1 = Scale(c1, e1);
            Vec c2 = Column(m, 2);
            c2 = Sub(c2, Scale(c1, Dot(c1, c2)));
            c2 = Sub(c2, Scale(c0, Dot(c0, c2)));

            if (const double sq2 = MagSq(c2); sq2 > bound) {
                s[2] = Nw4rMath::FSqrt(sq2);

                // A left-handed basis: all three negated (fmul by -1.0, frsp).
                if (Dot(c0, Cross(c1, c2)) < zero) {
                    const double flip = Memory::ReadFloat64(0x80798458u); // r2 - 31496

                    for (float& v : s)
                        v = float(double(v) * flip);
                }
            }
        } else {
            Vec c2 = Column(m, 2);
            c2 = Sub(c2, Scale(c0, Dot(c0, c2)));

            if (const double sq2 = MagSq(c2); sq2 > bound)
                s[2] = Nw4rMath::FSqrt(sq2);
        }
    } else {
        Vec c1 = Column(m, 1);

        if (const double sq1 = MagSq(c1); sq1 > bound) {
            const float e1 = Nw4rMath::FrSqrt(sq1);
            s[1] = Nw4rMath::FastReciprocal(e1);
            c1 = Scale(c1, e1);
            Vec c2 = Column(m, 2);
            c2 = Sub(c2, Scale(c1, Dot(c1, c2)));
            s[2] = PsVec::Mag(c2.x, c2.y, c2.z);
        } else {
            const Vec c2 = Column(m, 2);
            s[2] = PsVec::Mag(c2.x, c2.y, c2.z);
        }
    }

    for (uint32_t k = 0; k < 3; ++k)
        MemoryInline::FlatWriteFloat32(out + 4 * k, s[k]);
}

void GetScalePort(CpuContext* p) {
    GetScale(p->gpr[3], p->gpr[4]);
}

} // namespace

extern "C" void func_8009C0A4(CpuContext* ctx);
static void GetScaleChecked([[maybe_unused]] uint32_t r1, uint32_t r3, uint32_t r4) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("EfMtxGetScale")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = 0x8079FF60u;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        PortCheck::RunOutput(c, "EfMtxGetScale", func_8009C0A4, GetScalePort, {{r4, 12}});
        return;
    }
#endif
    GetScale(r3, r4);
}

extern "C" void MH3_EfMtxGetScale(CpuContext* ctx) {
    GetScaleChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009C0A4, MH3_EfMtxGetScale, "EfMtxGetScale");
