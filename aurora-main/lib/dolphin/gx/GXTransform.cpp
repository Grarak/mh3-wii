#include <aurora/legacy_gx.h>
#include "gx.hpp"
#include "__gx.h"
#include "dolphin/mtx/GeoTypes.h"

static inline void CacheProjectionVector(const f32* ptr, GXProjectionType type) {
  __gx->projType = type;
  for (int i = 0; i < 6; ++i) {
    __gx->projMtx[i] = ptr[i + 1];
  }
}

extern "C" {

void GXSetProjection(const void* mtx_, GXProjectionType type) {
  aurora_legacy_gx_panic("GXSetProjection");
}

void GXSetProjectionv(const f32* ptr) {
  aurora_legacy_gx_panic("GXSetProjectionv");
}

void GXLoadPosMtxImm(const void* mtx_, u32 id) {
  aurora_legacy_gx_panic("GXLoadPosMtxImm");
}

void GXLoadNrmMtxImm(const void* mtx_, u32 id) {
  aurora_legacy_gx_panic("GXLoadNrmMtxImm");
}

void GXSetCurrentMtx(u32 id) {
  aurora_legacy_gx_panic("GXSetCurrentMtx");
}

void GXLoadTexMtxImm(const void* mtx_, u32 id, GXTexMtxType type) {
  aurora_legacy_gx_panic("GXLoadTexMtxImm");
}

void GXSetViewport(float left, float top, float width, float height, float nearZ, float farZ) {
  aurora_legacy_gx_panic("GXSetViewport");
}

void GXSetViewportJitter(float left, float top, float width, float height, float nearZ, float farZ, u32 field) {
  aurora_legacy_gx_panic("GXSetViewportJitter");
}

void GXProject(f32 x, f32 y, f32 z, const f32 mtx[3][4], const f32* pm, const f32* vp, f32* sx,
               f32* sy, f32* sz) {
  aurora_legacy_gx_panic("GXProject");
}

// TODO GXLoadPosMtxIndx
// TODO GXLoadNrmMtxImm3x3
// TODO GXLoadNrmMtxIndx3x3
// TODO GXLoadTexMtxIndx
void GXSetZScaleOffset(f32 scale, f32 offset) {
  aurora_legacy_gx_panic("GXSetZScaleOffset");
}

void GXSetScissorBoxOffset(s32 x_off, s32 y_off) {
  aurora_legacy_gx_panic("GXSetScissorBoxOffset");
}
}
