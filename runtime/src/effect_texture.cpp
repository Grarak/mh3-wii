// Existing native address retained. Native texture resources, animation caches
// and matrix arithmetic use typed C inputs; no GX texture object is submitted.
#include "abi_bridge.h"
#include "native_effect_draw.h"
extern "C" void MH3_EffectDrawTextureSetup(CpuContext *ctx){ctx->gpr[3]=NativeEffectTextureSetup(ctx);}
REGISTER_NATIVE_FUNCTION_AS(0x800C7D44,MH3_EffectDrawTextureSetup,"EffectDrawTextureSetup");
