#include <aurora/legacy_gx.h>
#include "gx.hpp"
#include "__gx.h"
#include "dolphin/gd/GDGeometry.h"
#include "../../gx/fifo.hpp"

static inline void SETVCDATTR(GXAttr attr, GXAttrType type) {
  switch (attr) {
  case GX_VA_PNMTXIDX:
    SET_REG_FIELD(0, __gx->vcdLo, 1, 0, type);
    break;
  case GX_VA_TEX0MTXIDX:
    SET_REG_FIELD(0, __gx->vcdLo, 1, 1, type);
    break;
  case GX_VA_TEX1MTXIDX:
    SET_REG_FIELD(0, __gx->vcdLo, 1, 2, type);
    break;
  case GX_VA_TEX2MTXIDX:
    SET_REG_FIELD(0, __gx->vcdLo, 1, 3, type);
    break;
  case GX_VA_TEX3MTXIDX:
    SET_REG_FIELD(0, __gx->vcdLo, 1, 4, type);
    break;
  case GX_VA_TEX4MTXIDX:
    SET_REG_FIELD(0, __gx->vcdLo, 1, 5, type);
    break;
  case GX_VA_TEX5MTXIDX:
    SET_REG_FIELD(0, __gx->vcdLo, 1, 6, type);
    break;
  case GX_VA_TEX6MTXIDX:
    SET_REG_FIELD(0, __gx->vcdLo, 1, 7, type);
    break;
  case GX_VA_TEX7MTXIDX:
    SET_REG_FIELD(0, __gx->vcdLo, 1, 8, type);
    break;
  case GX_VA_POS:
    SET_REG_FIELD(0, __gx->vcdLo, 2, 9, type);
    break;
  case GX_VA_NRM:
    __gx->hasNrms = (type != 0);
    if (type != GX_NONE) {
      __gx->nrmType = type;
    }
    break;
  case GX_VA_NBT:
    __gx->hasBiNrms = (type != 0);
    if (type != GX_NONE) {
      __gx->nrmType = type;
    }
    break;
  case GX_VA_CLR0:
    SET_REG_FIELD(0, __gx->vcdLo, 2, 13, type);
    break;
  case GX_VA_CLR1:
    SET_REG_FIELD(0, __gx->vcdLo, 2, 15, type);
    break;
  case GX_VA_TEX0:
    SET_REG_FIELD(0, __gx->vcdHi, 2, 0, type);
    break;
  case GX_VA_TEX1:
    SET_REG_FIELD(0, __gx->vcdHi, 2, 2, type);
    break;
  case GX_VA_TEX2:
    SET_REG_FIELD(0, __gx->vcdHi, 2, 4, type);
    break;
  case GX_VA_TEX3:
    SET_REG_FIELD(0, __gx->vcdHi, 2, 6, type);
    break;
  case GX_VA_TEX4:
    SET_REG_FIELD(0, __gx->vcdHi, 2, 8, type);
    break;
  case GX_VA_TEX5:
    SET_REG_FIELD(0, __gx->vcdHi, 2, 10, type);
    break;
  case GX_VA_TEX6:
    SET_REG_FIELD(0, __gx->vcdHi, 2, 12, type);
    break;
  case GX_VA_TEX7:
    SET_REG_FIELD(0, __gx->vcdHi, 2, 14, type);
    break;
  default:
    break;
  }
}

static inline void SETVAT(u32* va, u32* vb, u32* vc, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
  switch (attr) {
  case GX_VA_POS:
    SET_REG_FIELD(0, *va, 1, 0, cnt);
    SET_REG_FIELD(0, *va, 3, 1, type);
    SET_REG_FIELD(0, *va, 5, 4, frac);
    break;
  case GX_VA_NRM:
  case GX_VA_NBT:
    SET_REG_FIELD(0, *va, 3, 10, type);
    if (cnt == GX_NRM_NBT3) {
      SET_REG_FIELD(0, *va, 1, 9, 1);
      SET_REG_FIELD(0, *va, 1, 31, 1);
    } else {
      SET_REG_FIELD(0, *va, 1, 9, cnt);
      SET_REG_FIELD(0, *va, 1, 31, 0);
    }
    break;
  case GX_VA_CLR0:
    SET_REG_FIELD(0, *va, 1, 13, cnt);
    SET_REG_FIELD(0, *va, 3, 14, type);
    break;
  case GX_VA_CLR1:
    SET_REG_FIELD(0, *va, 1, 17, cnt);
    SET_REG_FIELD(0, *va, 3, 18, type);
    break;
  case GX_VA_TEX0:
    SET_REG_FIELD(0, *va, 1, 21, cnt);
    SET_REG_FIELD(0, *va, 3, 22, type);
    SET_REG_FIELD(0, *va, 5, 25, frac);
    break;
  case GX_VA_TEX1:
    SET_REG_FIELD(0, *vb, 1, 0, cnt);
    SET_REG_FIELD(0, *vb, 3, 1, type);
    SET_REG_FIELD(0, *vb, 5, 4, frac);
    break;
  case GX_VA_TEX2:
    SET_REG_FIELD(0, *vb, 1, 9, cnt);
    SET_REG_FIELD(0, *vb, 3, 10, type);
    SET_REG_FIELD(0, *vb, 5, 13, frac);
    break;
  case GX_VA_TEX3:
    SET_REG_FIELD(0, *vb, 1, 18, cnt);
    SET_REG_FIELD(0, *vb, 3, 19, type);
    SET_REG_FIELD(0, *vb, 5, 22, frac);
    break;
  case GX_VA_TEX4:
    SET_REG_FIELD(0, *vb, 1, 27, cnt);
    SET_REG_FIELD(0, *vb, 3, 28, type);
    SET_REG_FIELD(0, *vc, 5, 0, frac);
    break;
  case GX_VA_TEX5:
    SET_REG_FIELD(0, *vc, 1, 5, cnt);
    SET_REG_FIELD(0, *vc, 3, 6, type);
    SET_REG_FIELD(0, *vc, 5, 9, frac);
    break;
  case GX_VA_TEX6:
    SET_REG_FIELD(0, *vc, 1, 14, cnt);
    SET_REG_FIELD(0, *vc, 3, 15, type);
    SET_REG_FIELD(0, *vc, 5, 18, frac);
    break;
  case GX_VA_TEX7:
    SET_REG_FIELD(0, *vc, 1, 23, cnt);
    SET_REG_FIELD(0, *vc, 3, 24, type);
    SET_REG_FIELD(0, *vc, 5, 27, frac);
    break;
  default:
    break;
  }
}

static inline bool AuroraVtxDescDiffers(GXAttr attr, GXAttrType type) {
  return attr >= GX_VA_PNMTXIDX && attr < GX_VA_MAX_ATTR && g_gxState.vtxDesc[attr] != type;
}

static inline bool AuroraVtxAttrFmtDiffers(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
  if (vtxfmt < GX_VTXFMT0 || vtxfmt >= GX_MAX_VTXFMT || attr < GX_VA_POS || attr >= GX_VA_MAX_ATTR) {
    return true;
  }
  const auto& fmt = g_gxState.vtxFmts[vtxfmt].attrs[attr];
  return fmt.cnt != cnt || fmt.type != type || fmt.frac != frac;
}

extern "C" {

void GXSetVtxDesc(GXAttr attr, GXAttrType type) {
  aurora_legacy_gx_panic("GXSetVtxDesc");
}

void GXSetSourceVtxDesc(GXAttr attr, GXAttrType type) {
  aurora_legacy_gx_panic("GXSetSourceVtxDesc");
}

void GXSetVtxDescv(GXVtxDescList* list) {
  aurora_legacy_gx_panic("GXSetVtxDescv");
}

void GXClearVtxDesc() {
  aurora_legacy_gx_panic("GXClearVtxDesc");
}

void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
  aurora_legacy_gx_panic("GXSetVtxAttrFmt");
}

void GXSetVtxAttrFmtv(GXVtxFmt vtxfmt, const GXVtxAttrFmtList* list) {
  aurora_legacy_gx_panic("GXSetVtxAttrFmtv");
}

void GXSetArray(GXAttr attr, const void* data, u32 size, u8 stride, bool le) {
  aurora_legacy_gx_panic("GXSetArray");
}

void GXSetTexCoordGen2(GXTexCoordID dst, GXTexGenType type, GXTexGenSrc src, u32 mtx, GXBool normalize, u32 postMtx) {
  aurora_legacy_gx_panic("GXSetTexCoordGen2");
}

void GXSetNumTexGens(u8 num) {
  aurora_legacy_gx_panic("GXSetNumTexGens");
}

void GXInvalidateVtxCache() {
  aurora_legacy_gx_panic("GXInvalidateVtxCache");
}

void GXSetLineWidth(u8 width, GXTexOffset offs) {
  aurora_legacy_gx_panic("GXSetLineWidth");
}

void GXSetPointSize(u8 pointSize, GXTexOffset texOffsets) {
  aurora_legacy_gx_panic("GXSetPointSize");
}

void GXEnableTexOffsets(GXTexCoordID coord, GXBool line_enable, GXBool point_enable) {
  aurora_legacy_gx_panic("GXEnableTexOffsets");
}
}
