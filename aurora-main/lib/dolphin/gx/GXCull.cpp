#include <aurora/legacy_gx.h>
#include "gx.hpp"
#include "__gx.h"

extern "C" {

void GXSetScissor(u32 left, u32 top, u32 width, u32 height) {
  aurora_legacy_gx_panic("GXSetScissor");
}

// TODO GXSetScissorBoxOffset

void GXSetCullMode(GXCullMode mode) {
  aurora_legacy_gx_panic("GXSetCullMode");
}

void GXSetClipMode(GXClipMode mode) {
  aurora_legacy_gx_panic("GXSetClipMode");
}

void GXSetCoPlanar(GXBool enable) {
  aurora_legacy_gx_panic("GXSetCoPlanar");
}
}
