// MSL math, RMHP08: the fdlibm routines the game calls per frame, on the host
// libm. sqrt is correctly rounded on both sides, so it matches bit for bit;
// atan2 is within an ulp of fdlibm's. The float wrappers round the double
// result to single precision, as the guest's frsp does.
//
//   0x804685FC sqrt         f1 = sqrt(f1)
//   0x804645F4 sqrtf        f1 = (float)sqrt(f1)
//   0x80464BE0 atan2        f1 = atan2(f1, f2) (__ieee754_atan2, y then x)
//   0x80464488 atan2f       f1 = (float)atan2(f1, f2)
//
// nw4r::math, also on the host:
//
//   0x8009C7C4 SinCosRad    (f32* sin, f32* cos, f1 radians)
//
// and from nw4r::ef (ef_util.cpp), on the same sine table:
//
//   0x8009C838 RotationYAxis (const VEC3* radians, VEC3* out) -> out
//   0x8009CA94 RotationMtx   (MTX34* out, f1 f2 f3 radians) -> out
//
// and without it:
//
//   0x8009C754 SinCosByPeriod (f32 out[2], f1 radians): RotationYAxis's
//              lookup with the period at 0x80798468
//   0x8009B4AC MtxFromDirection (MTX34* out, const VEC3* unit)
//   0x8009C4E8 VecNormalize  (VEC3* dst, const VEC3* src) -> not zero
//   0x8009CC04 MtxScaleColumns (MTX34* out, const MTX34* m, const VEC3* s)
//   0x8009CC84 MtxScaleRows  (MTX34* out, const VEC3* s, const MTX34* m)
//
// fdlibm also stores EDOM to errno (0x807972C8) for a NaN, infinite or
// negative sqrt argument; the port drops that store.
#include "abi_bridge.h"
#include "effect_particle.h"
#include "fatal.h"
#include "memory.h"
#include "nw4r_math.h"
#include "port_check.h"
#include "ppc_runtime.h"
#include "ps_vec_math.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

extern "C" void MH3_Sqrt(CpuContext* ctx) {
    ctx->fpr[1].d = std::sqrt(ctx->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x804685FC, MH3_Sqrt, "sqrt");

extern "C" void MH3_Sqrtf(CpuContext* ctx) {
    ctx->fpr[1].d = double(float(std::sqrt(ctx->fpr[1].d)));
}
REGISTER_NATIVE_FUNCTION_AS(0x804645F4, MH3_Sqrtf, "sqrtf");

extern "C" void MH3_Atan2(CpuContext* ctx) {
    ctx->fpr[1].d = std::atan2(ctx->fpr[1].d, ctx->fpr[2].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x80464BE0, MH3_Atan2, "atan2");

extern "C" void MH3_Atan2f(CpuContext* ctx) {
    ctx->fpr[1].d = double(float(std::atan2(ctx->fpr[1].d, ctx->fpr[2].d)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80464488, MH3_Atan2f, "atan2f");

// SinCosRad: the angle in 1/256 turns (times 256 / 2 pi), its magnitude
// wrapped below 65536 and cut to an index into the 256 entries {sin, cos,
// dsin, dcos} at 0x805744A0; each value interpolated with the fraction (the
// paired-single madd, one rounding) and the sine negated for a negative angle.
void SinCosRadHost(double radians, float& sine, float& cosine) {
    constexpr uint32_t kTable = 0x805744A0;
    const float index = float(radians) * MemoryInline::FlatReadFloat32(0x8079FF60 - 31528); // 256 / 2 pi
    float magnitude = std::fabs(index);

    while (magnitude >= 65536.0f)
        magnitude -= 65536.0f;

    const uint32_t whole = uint32_t(magnitude);
    const float fraction = magnitude - float(whole & 0xFFFF);
    const uint32_t entry = kTable + 16 * (whole & 0xFF);
    const float s = float(PPC_Fmadds(MemoryInline::FlatReadFloat32(entry + 8), fraction, MemoryInline::FlatReadFloat32(entry + 0)));
    cosine = float(PPC_Fmadds(MemoryInline::FlatReadFloat32(entry + 12), fraction, MemoryInline::FlatReadFloat32(entry + 4)));
    sine = index < 0.0f ? -s : s;
}

static void SinCosRadPort(CpuContext* ctx) {
    float sine, cosine;
    SinCosRadHost(ctx->fpr[1].d, sine, cosine);
    Memory::WriteFloat32(ctx->gpr[4], cosine);
    Memory::WriteFloat32(ctx->gpr[3], sine);
}

extern "C" void func_8009C7C4(CpuContext* ctx);
extern "C" void MH3_SinCosRad(CpuContext* ctx) {
    PortCheck::Call(ctx, "SinCosRad", func_8009C7C4, SinCosRadPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009C7C4, MH3_SinCosRad, "SinCosRad");

// 0x8009C838 (ef_util.cpp), called per particle by the field kinds 0, 4 and 6:
// out = the Y axis of the rotation by the angles (x, y, z) radians,
//   (cz sy sx - sz cx, sz sy sx + cz cx, cy sx)
// each sine and cosine looked up as SinCosRad does, but with the original's
// wrap (only a magnitude past the period is reduced) and its u16 index
// conversion (GQR3, saturating). The multiplies and multiply-adds are the
// paired-single lanes' (float, and on ARM32 unfused, as the translated code
// does them). Returns out. The registered body names only the registers it
// uses, so translated callers call it directly.
extern "C" void func_8009C838(CpuContext* ctx);
namespace {
constexpr uint32_t kSinCosTable = 0x805744A0;
constexpr uint32_t kRad2Index = 0x80798438; // 256 / 2 pi
constexpr uint32_t kIndexPeriod = 0x80798430;

struct SinCos {
    float s, c;
};

SinCos LookUp(float index, float period) {
    float magnitude = std::fabs(index);

    if (magnitude > period) {
        do
            magnitude -= period;
        while (magnitude >= period);
    }

    const uint32_t whole = magnitude >= 65535.0f ? 65535u : magnitude > 0.0f ? uint32_t(magnitude) : 0u;
    const float fraction = magnitude - float(whole);
    const uint32_t entry = kSinCosTable + 16 * (whole & 0xFF);
    float s = PpcFmaFloatInline(MemoryInline::FlatReadFloat32(entry + 8), fraction, MemoryInline::FlatReadFloat32(entry + 0));
    const float c = PpcFmaFloatInline(MemoryInline::FlatReadFloat32(entry + 12), fraction, MemoryInline::FlatReadFloat32(entry + 4));
#if defined(__aarch64__)
    // AArch64 compilers fold the negation into the fused multiply-add
    // (FNMADD, -(a b) - c), which makes a zero sine +0 where ps_neg makes it
    // -0 (an angle of -pi): keep them apart.
    __asm__("" : "+w"(s));
#endif
    return {index < 0.0f ? -s : s, c};
}

uint32_t RotationYAxis(uint32_t angles, uint32_t out) {
    // NW4R's pOut check always passes; the pointer is folded instead.
    out = GuestFlat::CachedAlias(out);

    const float k = MemoryInline::FlatReadFloat32(kRad2Index), period = MemoryInline::FlatReadFloat32(kIndexPeriod);
    const SinCos x = LookUp(MemoryInline::FlatReadFloat32(angles) * k, period);
    const SinCos y = LookUp(MemoryInline::FlatReadFloat32(angles + 4) * k, period);
    const SinCos z = LookUp(MemoryInline::FlatReadFloat32(angles + 8) * k, period);
    const float sysx = y.s * x.s, cysx = y.c * x.s;
    const float szcx = z.s * x.c, czcx = z.c * x.c;
    MemoryInline::FlatWriteFloat32(out, PpcFmaFloatInline(z.c, sysx, -szcx));
    MemoryInline::FlatWriteFloat32(out + 4, PpcFmaFloatInline(z.s, sysx, czcx));
    MemoryInline::FlatWriteFloat32(out + 8, cysx);
    return out;
}

void RotationYAxisPort(CpuContext* ctx) {
    ctx->gpr[3] = RotationYAxis(ctx->gpr[3], ctx->gpr[4]);
}

uint32_t RotationYAxisChecked(uint32_t r1, uint32_t r3, uint32_t r4) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("RotationYAxis");

    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        PortCheck::RunOutput(c, "RotationYAxis", func_8009C838, RotationYAxisPort, {{r4, 12}});
        return c->gpr[3];
    }
#else
    (void)r1;
#endif
    return RotationYAxis(r3, r4);
}
} // namespace

extern "C" void MH3_RotationYAxis(CpuContext* ctx) {
    ctx->gpr[3] = RotationYAxisChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009C838, MH3_RotationYAxis, "RotationYAxis");

namespace {
// 0x8009CA94 (ef_util.cpp), the emitters' rotation and the particle managers'
// wrap matrix: out = Rz Ry Rx of the angles (x, y, z) radians,
//   cz cy   sx sy cz - cx sz   cx sy cz + sx sz   0
//   sz cy   sx sy sz + cx cz   cx sy sz - sx cz   0
//   -sy     sx cy              cx cy              0
// each sine and cosine looked up as RotationYAxis does, the products and
// multiply-adds the paired-single lanes' in the original's grouping.
void RotationMtx(uint32_t out, float x, float y, float z) {
    const float k = MemoryInline::FlatReadFloat32(kRad2Index), period = MemoryInline::FlatReadFloat32(kIndexPeriod);
    const SinCos rx = LookUp(x * k, period), ry = LookUp(y * k, period), rz = LookUp(z * k, period);
    const float m[12] = {
        rz.c * ry.c,
        PpcFmaFloatInline(rx.s * rz.c, ry.s, -rx.c * rz.s),
        PpcFmaFloatInline(rx.c * rz.c, ry.s, rx.s * rz.s),
        0.0f,
        rz.s * ry.c,
        PpcFmaFloatInline(rx.s * rz.s, ry.s, rx.c * rz.c),
        PpcFmaFloatInline(rx.c * rz.s, ry.s, -(rx.s * rz.c)),
        0.0f,
        -ry.s,
        rx.s * ry.c,
        rx.c * ry.c,
        0.0f,
    };

    for (uint32_t i = 0; i < 12; ++i)
        MemoryInline::FlatWriteFloat32(out + 4 * i, m[i]);
}

// 0x8009C4E8 (ef_util.cpp): dst = src / |src| by the Gekko's reciprocal
// square root estimate (frsqrte) and one Newton step, r (3 - r r l) / 2 (3
// at 0x80798464, 0.5 at 0x80798460); a zero vector is copied. Whether it was
// not zero. The original's pointer checks always pass and are dropped.
bool NormalizeHost(float v[3]) {
    const float x = v[0], y = v[1], z = v[2];
    const float xx = x * x, yy = y * y;
    const float length2 = PpcFmaFloatInline(z, z, xx) + yy;

    if (length2 == 0.0f)
        return false;

    const double estimate = PPC_Frsqrte(length2);
    const double half = PpcFmulsInline(estimate, MemoryInline::FlatReadFloat32(0x80798460));
    const double step = PPC_Fnmsubs(PpcFmulsInline(estimate, estimate), length2, MemoryInline::FlatReadFloat32(0x80798464));
    const float scale = float(PpcFmulsInline(step, half));
    v[0] = x * scale;
    v[1] = y * scale;
    v[2] = z * scale;
    return true;
}

bool VecNormalize(uint32_t dst, uint32_t src) {
    float v[3];

    for (uint32_t k = 0; k < 3; ++k)
        v[k] = MemoryInline::FlatReadFloat32(src + 4 * k);

    if (!NormalizeHost(v)) {
        for (uint32_t k = 0; k < 12; k += 4)
            MemoryInline::FlatWrite32(dst + k, MemoryInline::FlatRead32(src + k));

        return false;
    }

    for (uint32_t k = 0; k < 3; ++k)
        MemoryInline::FlatWriteFloat32(dst + 4 * k, v[k]);

    return true;
}

// 0x8009C754 (ef_util.cpp): the sine and cosine of `radians` looked up as
// RotationYAxis does, wrapped by the period at 0x80798468.
SinCos SinCosByPeriod(double radians) {
    const float index = float(radians) * MemoryInline::FlatReadFloat32(kRad2Index);
    return LookUp(index, MemoryInline::FlatReadFloat32(0x80798468));
}

// 0x8009B4AC (ef_util.cpp): the rotation (no translation) taking the Y axis
// to the unit vector v = (x, y, z), with s = sqrt(1 - z z) (FSqrt):
//   (y/s  x  -z x/s  0)
//   (-x/s y  -y z/s  0)
//   (0    z   s      0)
// and the identity's x and y columns for z at the poles (1 - |z| below
// 0x8079844C). It asserts |v| is 1 within 0x80798444 (the one and zero at
// 0x80798440 and 0x8079843C).
void MtxFromDirection(const float v[3], float m[12]) {
    const float one = MemoryInline::FlatReadFloat32(0x80798440), zero = MemoryInline::FlatReadFloat32(0x8079843C);
    const float off = PsVec::Mag(v[0], v[1], v[2]) - one;

    if (!(std::fabs(off) < MemoryInline::FlatReadFloat32(0x80798444)))
        MH3_THROW(std::runtime_error("NW4R panic (ef_util.cpp:431): Failed assertion nw4r::math::FAbs(VECLen(v) - 1) < epsilon"));

    const float x = v[0], y = v[1], z = v[2];
    float a = one, b = zero, s = zero;

    if (!(one - std::fabs(z) < MemoryInline::FlatReadFloat32(0x8079844C))) {
        const float zz = z * z;
        s = Nw4rMath::FSqrt(one - zz);
        a = y / s;
        b = x / -s;
    }

    const float bz = z * b, az = -a * z;
    const float out[12] = {a, x, bz, zero, b, y, az, zero, zero, z, s, zero};

    for (uint32_t i = 0; i < 12; ++i)
        m[i] = out[i];
}

// 0x8009CC04 (ef_util.cpp), the emitters' scale: out = m with its first
// three columns scaled by s, a zero component taken as 0x80798444; the
// translation column kept. out may be m.
void MtxScaleColumns(uint32_t out, uint32_t m, uint32_t scale) {
    const float fallback = MemoryInline::FlatReadFloat32(0x80798444);
    float s[3];

    for (uint32_t c = 0; c < 3; ++c) {
        s[c] = MemoryInline::FlatReadFloat32(scale + 4 * c);

        if (s[c] == 0.0f)
            s[c] = fallback;
    }

    float v[12];

    for (uint32_t i = 0; i < 12; ++i)
        v[i] = MemoryInline::FlatReadFloat32(m + 4 * i);

    for (uint32_t r = 0; r < 3; ++r)
        for (uint32_t c = 0; c < 3; ++c)
            v[4 * r + c] *= s[c];

    for (uint32_t i = 0; i < 12; ++i)
        MemoryInline::FlatWriteFloat32(out + 4 * i, v[i]);
}

// 0x8009CC84 (ef_util.cpp), the particle managers' wrap matrix: out = m with
// its rows scaled by s (a zero component taken as 0x80798470). out may be m.
void MtxScaleRows(uint32_t out, uint32_t scale, uint32_t m) {
    const float fallback = MemoryInline::FlatReadFloat32(0x80798470);
    float v[12];

    for (uint32_t i = 0; i < 12; ++i)
        v[i] = MemoryInline::FlatReadFloat32(m + 4 * i);

    for (uint32_t r = 0; r < 3; ++r) {
        float s = MemoryInline::FlatReadFloat32(scale + 4 * r);

        if (s == 0.0f)
            s = fallback;

        for (uint32_t c = 0; c < 4; ++c)
            v[4 * r + c] *= s;
    }

    for (uint32_t i = 0; i < 12; ++i)
        MemoryInline::FlatWriteFloat32(out + 4 * i, v[i]);
}

void RotationMtxPort(CpuContext* ctx) {
    RotationMtx(ctx->gpr[3], float(ctx->fpr[1].d), float(ctx->fpr[2].d), float(ctx->fpr[3].d));
}

void VecNormalizePort(CpuContext* ctx) {
    ctx->gpr[3] = VecNormalize(ctx->gpr[3], ctx->gpr[4]);
}

void MtxScaleColumnsPort(CpuContext* ctx) {
    MtxScaleColumns(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}

void MtxScaleRowsPort(CpuContext* ctx) {
    MtxScaleRows(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}

void SinCosByPeriodPort(CpuContext* ctx) {
    const SinCos r = SinCosByPeriod(ctx->fpr[1].d);
    MemoryInline::FlatWriteFloat32(ctx->gpr[3], r.s);
    MemoryInline::FlatWriteFloat32(ctx->gpr[3] + 4, r.c);
}

// NW4R_POINTER_CHECK of the matrix: MEM1, MEM2, their uncached mirrors or
// the locked cache.
void MtxFromDirectionPort(CpuContext* ctx) {
    const uint32_t out = ctx->gpr[3];

    if ((out & 0xFF000000) != 0x80000000 && (out & 0xFF800000) != 0x81000000 && (out & 0xF8000000) != 0x90000000 && (out & 0xFF000000) != 0xC0000000 &&
        (out & 0xFF800000) != 0xC1000000 && (out & 0xF8000000) != 0xD0000000 && (out & 0xFFFFC000) != 0xE0000000)
        MH3_THROW(std::runtime_error("NW4R panic (ef_util.cpp:430): Pointer Error: mtx is not valid"));

    float v[3], m[12];

    for (uint32_t k = 0; k < 3; ++k)
        v[k] = MemoryInline::FlatReadFloat32(ctx->gpr[4] + 4 * k);

    MtxFromDirection(v, m);

    for (uint32_t i = 0; i < 12; ++i)
        MemoryInline::FlatWriteFloat32(out + 4 * i, m[i]);
}
} // namespace

void EfRotationMtx(uint32_t out, float x, float y, float z) {
    RotationMtx(out, x, y, z);
}

bool EfVecNormalize(uint32_t dst, uint32_t src) {
    return VecNormalize(dst, src);
}

void EfMtxScaleColumns(uint32_t out, uint32_t m, uint32_t scale) {
    MtxScaleColumns(out, m, scale);
}

void EfMtxScaleRows(uint32_t out, uint32_t scale, uint32_t m) {
    MtxScaleRows(out, scale, m);
}

bool EfNormalize(float v[3]) {
    return NormalizeHost(v);
}

void EfSinCosByPeriod(double radians, float& sine, float& cosine) {
    const SinCos r = SinCosByPeriod(radians);
    sine = r.s;
    cosine = r.c;
}

void EfMtxFromDirection(const float v[3], float m[12]) {
    MtxFromDirection(v, m);
}

// MH3_PORT_CHECK=Name compares each port's output (and VecNormalize's r3) on
// every call.
extern "C" void func_8009CA94(CpuContext* ctx);
extern "C" void MH3_RotationMtx(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("RotationMtx")) [[unlikely]]
        return PortCheck::RunOutput(ctx, "RotationMtx", func_8009CA94, RotationMtxPort, {{ctx->gpr[3], 48}});
#endif
    RotationMtxPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009CA94, MH3_RotationMtx, "RotationMtx");

extern "C" void func_8009C4E8(CpuContext* ctx);
extern "C" void MH3_VecNormalize(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("VecNormalize")) [[unlikely]]
        return PortCheck::RunOutput(ctx, "VecNormalize", PortCheck::Returns::Gpr3, func_8009C4E8, VecNormalizePort, {{ctx->gpr[3], 12}});
#endif
    VecNormalizePort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009C4E8, MH3_VecNormalize, "VecNormalize");

extern "C" void func_8009CC04(CpuContext* ctx);
extern "C" void MH3_MtxScaleColumns(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("MtxScaleColumns")) [[unlikely]]
        return PortCheck::RunOutput(ctx, "MtxScaleColumns", func_8009CC04, MtxScaleColumnsPort, {{ctx->gpr[3], 48}});
#endif
    MtxScaleColumnsPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009CC04, MH3_MtxScaleColumns, "MtxScaleColumns");

extern "C" void func_8009CC84(CpuContext* ctx);
extern "C" void MH3_MtxScaleRows(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("MtxScaleRows")) [[unlikely]]
        return PortCheck::RunOutput(ctx, "MtxScaleRows", func_8009CC84, MtxScaleRowsPort, {{ctx->gpr[3], 48}});
#endif
    MtxScaleRowsPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009CC84, MH3_MtxScaleRows, "MtxScaleRows");

extern "C" void func_8009C754(CpuContext* ctx);
extern "C" void MH3_SinCosByPeriod(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("SinCosByPeriod")) [[unlikely]]
        return PortCheck::RunOutput(ctx, "SinCosByPeriod", func_8009C754, SinCosByPeriodPort, {{ctx->gpr[3], 8}});
#endif
    SinCosByPeriodPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009C754, MH3_SinCosByPeriod, "SinCosByPeriod");

extern "C" void func_8009B4AC(CpuContext* ctx);
extern "C" void MH3_MtxFromDirection(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("MtxFromDirection")) [[unlikely]]
        return PortCheck::RunOutput(ctx, "MtxFromDirection", func_8009B4AC, MtxFromDirectionPort, {{ctx->gpr[3], 48}});
#endif
    MtxFromDirectionPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009B4AC, MH3_MtxFromDirection, "MtxFromDirection");
