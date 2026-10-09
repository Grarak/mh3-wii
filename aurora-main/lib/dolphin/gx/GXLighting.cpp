#include <aurora/legacy_gx.h>
#include "gx.hpp"
#include "__gx.h"

extern "C" {
void GXInitLightAttn(GXLightObj* light_, float a0, float a1, float a2, float k0, float k1, float k2) {
  aurora_legacy_gx_panic("GXInitLightAttn");
}

void GXInitLightAttnA(GXLightObj* light_, float a0, float a1, float a2) {
  aurora_legacy_gx_panic("GXInitLightAttnA");
}

void GXInitLightAttnK(GXLightObj* light_, float k0, float k1, float k2) {
  aurora_legacy_gx_panic("GXInitLightAttnK");
}

void GXInitLightSpot(GXLightObj* light_, float cutoff, GXSpotFn spotFn) {
  aurora_legacy_gx_panic("GXInitLightSpot");
}

void GXInitLightDistAttn(GXLightObj* light_, float refDistance, float refBrightness, GXDistAttnFn distFunc) {
  aurora_legacy_gx_panic("GXInitLightDistAttn");
}

void GXInitLightPos(GXLightObj* light_, float x, float y, float z) {
  aurora_legacy_gx_panic("GXInitLightPos");
}

void GXInitLightColor(GXLightObj* light_, GXColor col) {
  aurora_legacy_gx_panic("GXInitLightColor");
}

void GXLoadLightObjImm(GXLightObj* light_, GXLightID id) {
  aurora_legacy_gx_panic("GXLoadLightObjImm");
}

void GXSetChanAmbColor(GXChannelID id, GXColor color) {
  aurora_legacy_gx_panic("GXSetChanAmbColor");
}

void GXSetChanMatColor(GXChannelID id, GXColor color) {
  aurora_legacy_gx_panic("GXSetChanMatColor");
}

void GXSetNumChans(u8 num) {
  aurora_legacy_gx_panic("GXSetNumChans");
}

void GXInitLightDir(GXLightObj* light_, float nx, float ny, float nz) {
  aurora_legacy_gx_panic("GXInitLightDir");
}

void GXInitSpecularDir(GXLightObj* light_, float nx, float ny, float nz) {
  aurora_legacy_gx_panic("GXInitSpecularDir");
}

void GXInitSpecularDirHA(GXLightObj* light_, float nx, float ny, float nz, float hx, float hy, float hz) {
  aurora_legacy_gx_panic("GXInitSpecularDirHA");
}

void GXSetChanCtrl(GXChannelID id, bool lightingEnabled, GXColorSrc ambSrc, GXColorSrc matSrc, u32 lightState,
                   GXDiffuseFn diffFn, GXAttnFn attnFn) {
  aurora_legacy_gx_panic("GXSetChanCtrl");
}
}
