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
// fdlibm also stores EDOM to errno (0x807972C8) for a NaN, infinite or
// negative sqrt argument; the port drops that store.
#include "abi_bridge.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cmath>

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
