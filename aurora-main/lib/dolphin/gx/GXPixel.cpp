#include <aurora/legacy_gx.h>
#include "gx.hpp"
#include "__gx.h"

#include <cmath>

extern "C" {
void GXSetFog(GXFogType type, float startZ, float endZ, float nearZ, float farZ, GXColor color) {
  aurora_legacy_gx_panic("GXSetFog");
}

void GXSetFogColor(GXColor color) {
  aurora_legacy_gx_panic("GXSetFogColor");
}

void GXSetFogRangeAdj(GXBool enable, u16 center, GXFogAdjTable* table) {
  aurora_legacy_gx_panic("GXSetFogRangeAdj");
}

void GXSetBlendMode(GXBlendMode mode, GXBlendFactor src, GXBlendFactor dst, GXLogicOp op) {
  aurora_legacy_gx_panic("GXSetBlendMode");
}

void GXSetColorUpdate(GXBool enabled) {
  aurora_legacy_gx_panic("GXSetColorUpdate");
}

void GXSetAlphaUpdate(bool enabled) {
  aurora_legacy_gx_panic("GXSetAlphaUpdate");
}

void GXSetZMode(bool compare_enable, GXCompare func, bool update_enable) {
  aurora_legacy_gx_panic("GXSetZMode");
}

void GXSetZCompLoc(GXBool before_tex) {
  aurora_legacy_gx_panic("GXSetZCompLoc");
}

void GXSetPixelFmt(GXPixelFmt pix_fmt, GXZFmt16 z_fmt) {
  aurora_legacy_gx_panic("GXSetPixelFmt");
}

void GXSetDither(GXBool dither) {
  aurora_legacy_gx_panic("GXSetDither");
}

void GXSetDstAlpha(bool enabled, u8 value) {
  aurora_legacy_gx_panic("GXSetDstAlpha");
}

void GXSetFieldMask(GXBool odd_mask, GXBool even_mask) {
  aurora_legacy_gx_panic("GXSetFieldMask");
}

void GXSetFieldMode(GXBool field_mode, GXBool half_aspect_ratio) {
  aurora_legacy_gx_panic("GXSetFieldMode");
}
}
