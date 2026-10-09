// Complete typed native particle traversal/geometry lives in mh3_effect_draw.c;
// the adapter marshals the game's manager, resource and view records.
#include "abi_bridge.h"
#include "native_effect_draw.h"
extern "C" void MH3_DrawBillboard(CpuContext *ctx){NativeEffectDrawBillboards(ctx);}
REGISTER_NATIVE_FUNCTION_AS(0x800B9E5C,MH3_DrawBillboard,"DrawBillboard");
