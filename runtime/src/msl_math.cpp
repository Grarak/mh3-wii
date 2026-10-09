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
//
// fdlibm also stores EDOM to errno (0x807972C8) for a NaN, infinite or
// negative sqrt argument; the port drops that store.
#include "abi_bridge.h"
#include "fatal.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

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

bool ValidOut(uint32_t p) {
    return (p & 0xFF000000u) == 0x80000000u || (p & 0xFF800000u) == 0x81000000u || (p & 0xF8000000u) == 0x90000000u || (p & 0xFF000000u) == 0xC0000000u ||
           (p & 0xFF800000u) == 0xC1000000u || (p & 0xF8000000u) == 0xD0000000u || (p & 0xFFFFC000u) == 0xE0000000u;
}

uint32_t RotationYAxis(uint32_t angles, uint32_t out) {
    if (!ValidOut(out)) [[unlikely]] {
        char text[96];
        std::snprintf(text, sizeof text, "NW4R panic (ef_util.cpp:934): Pointer Error pOut(=%08x) is not valid", out);
        MH3_THROW(std::runtime_error(text));
    }

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

    if (check && ValidOut(r4)) [[unlikely]] {
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
