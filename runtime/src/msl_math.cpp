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
// fdlibm also stores EDOM to errno (0x807972C8) for a NaN, infinite or
// negative sqrt argument; the port drops that store.
#include "abi_bridge.h"
#include "ppc_runtime.h"

#include <cmath>

extern "C" void MH3_Sqrt(CpuContext* ctx) { ctx->fpr[1].d = std::sqrt(ctx->fpr[1].d); }
REGISTER_NATIVE_FUNCTION_AS(0x804685FC, MH3_Sqrt, "sqrt");

extern "C" void MH3_Sqrtf(CpuContext* ctx) { ctx->fpr[1].d = double(float(std::sqrt(ctx->fpr[1].d))); }
REGISTER_NATIVE_FUNCTION_AS(0x804645F4, MH3_Sqrtf, "sqrtf");

extern "C" void MH3_Atan2(CpuContext* ctx) { ctx->fpr[1].d = std::atan2(ctx->fpr[1].d, ctx->fpr[2].d); }
REGISTER_NATIVE_FUNCTION_AS(0x80464BE0, MH3_Atan2, "atan2");

extern "C" void MH3_Atan2f(CpuContext* ctx) {
    ctx->fpr[1].d = double(float(std::atan2(ctx->fpr[1].d, ctx->fpr[2].d)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80464488, MH3_Atan2f, "atan2f");
