#include <aurora/legacy_gx.h>
#include "gx.hpp"
#include "__gx.h"

#include "../../gfx/common.hpp"
#include "../../gfx/texture.hpp"

extern "C" {

static inline u8 GetFracForNrm(GXCompType type) {
  switch (type) {
  case GX_S8:
    return 6;
  case GX_S16:
    return 14;
  default:
    return 0;
  }
}

void GXGetVtxDesc(GXAttr attr, GXAttrType* type) {
  aurora_legacy_gx_panic("GXGetVtxDesc");
}

void GXGetVtxDescv(GXVtxDescList* vcd) {
  aurora_legacy_gx_panic("GXGetVtxDescv");
}

void GXGetVtxAttrFmt(GXVtxFmt idx, GXAttr attr, GXCompCnt* compCnt, GXCompType* compType, u8* shift) {
  aurora_legacy_gx_panic("GXGetVtxAttrFmt");
}

void GXGetVtxAttrFmtv(GXVtxFmt fmt, GXVtxAttrFmtList* vat) {
  aurora_legacy_gx_panic("GXGetVtxAttrFmtv");
}

void GXGetLineWidth(u8* width, GXTexOffset* texOffsets) {
  aurora_legacy_gx_panic("GXGetLineWidth");
}

void GXGetPointSize(u8* pointSize, GXTexOffset* texOffsets) {
  aurora_legacy_gx_panic("GXGetPointSize");
}

void GXGetViewportv(f32 *vp) {
  aurora_legacy_gx_panic("GXGetViewportv");
}

void GXGetProjectionv(f32* p) {
  aurora_legacy_gx_panic("GXGetProjectionv");
}

void GXGetScissor(u32* left, u32* top, u32* wd, u32* ht) {
  aurora_legacy_gx_panic("GXGetScissor");
}

void GXGetCullMode(GXCullMode* mode) {
  aurora_legacy_gx_panic("GXGetCullMode");
}

void GXGetLightAttnA(GXLightObj* light_, float* a0, float* a1, float* a2) {
  aurora_legacy_gx_panic("GXGetLightAttnA");
}

void GXGetLightAttnK(GXLightObj* light_, float* k0, float* k1, float* k2) {
  aurora_legacy_gx_panic("GXGetLightAttnK");
}

void GXGetLightPos(GXLightObj* light_, float* x, float* y, float* z) {
  aurora_legacy_gx_panic("GXGetLightPos");
}

void GXGetLightDir(GXLightObj* light_, float* nx, float* ny, float* nz) {
  aurora_legacy_gx_panic("GXGetLightDir");
}

void GXGetLightColor(GXLightObj* light_, GXColor* col) {
  aurora_legacy_gx_panic("GXGetLightColor");
}

void* GXGetTexObjData(GXTexObj* tex_obj) {
  aurora_legacy_gx_panic("GXGetTexObjData");
}

u16 GXGetTexObjWidth(GXTexObj* tex_obj) {
  aurora_legacy_gx_panic("GXGetTexObjWidth");
}

u16 GXGetTexObjHeight(GXTexObj* tex_obj) {
  aurora_legacy_gx_panic("GXGetTexObjHeight");
}

GXTexFmt GXGetTexObjFmt(GXTexObj* tex_obj) {
  aurora_legacy_gx_panic("GXGetTexObjFmt");
}

GXTexWrapMode GXGetTexObjWrapS(GXTexObj* tex_obj) {
  aurora_legacy_gx_panic("GXGetTexObjWrapS");
}

GXTexWrapMode GXGetTexObjWrapT(GXTexObj* tex_obj) {
  aurora_legacy_gx_panic("GXGetTexObjWrapT");
}

GXBool GXGetTexObjMipMap(GXTexObj* tex_obj) {
  aurora_legacy_gx_panic("GXGetTexObjMipMap");
}

// TODO GXGetTexObjAll
// TODO GXGetTexObjMinFilt
// TODO GXGetTexObjMagFilt
// TODO GXGetTexObjMinLOD
// TODO GXGetTexObjMaxLOD
// TODO GXGetTexObjLODBias
// TODO GXGetTexObjBiasClamp
// TODO GXGetTexObjEdgeLOD
// TODO GXGetTexObjMaxAniso
// TODO GXGetTexObjLODAll
u32 GXGetTexObjTlut(const GXTexObj* tex_obj) {
  aurora_legacy_gx_panic("GXGetTexObjTlut");
}

// TODO GXGetTlutObjData
// TODO GXGetTlutObjFmt
// TODO GXGetTlutObjNumEntries
// TODO GXGetTlutObjAll
// TODO GXGetTexRegionAll
// TODO GXGetTlutRegionAll
}
