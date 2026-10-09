#include <aurora/legacy_gx.h>
#include "gx.hpp"
#include "__gx.h"

extern "C" {
void GXSetTevOp(GXTevStageID id, GXTevMode mode) {
  aurora_legacy_gx_panic("GXSetTevOp");
}

void GXSetTevColorIn(GXTevStageID stageId, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
  aurora_legacy_gx_panic("GXSetTevColorIn");
}

void GXSetTevAlphaIn(GXTevStageID stageId, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
  aurora_legacy_gx_panic("GXSetTevAlphaIn");
}

void GXSetTevColorOp(GXTevStageID stageId, GXTevOp op, GXTevBias bias, GXTevScale scale, bool clamp,
                     GXTevRegID outReg) {
  aurora_legacy_gx_panic("GXSetTevColorOp");
}

void GXSetTevAlphaOp(GXTevStageID stageId, GXTevOp op, GXTevBias bias, GXTevScale scale, bool clamp,
                     GXTevRegID outReg) {
  aurora_legacy_gx_panic("GXSetTevAlphaOp");
}

void GXSetTevColor(GXTevRegID id, GXColor color) {
  aurora_legacy_gx_panic("GXSetTevColor");
}

void GXSetTevColorS10(GXTevRegID id, GXColorS10 color) {
  aurora_legacy_gx_panic("GXSetTevColorS10");
}

void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
  aurora_legacy_gx_panic("GXSetAlphaCompare");
}

void GXSetTevOrder(GXTevStageID id, GXTexCoordID tcid, GXTexMapID tmid, GXChannelID cid) {
  aurora_legacy_gx_panic("GXSetTevOrder");
}

void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias) {
  aurora_legacy_gx_panic("GXSetZTexture");
}

void GXSetNumTevStages(u8 num) {
  aurora_legacy_gx_panic("GXSetNumTevStages");
}

void GXSetTevKColor(GXTevKColorID id, GXColor color) {
  aurora_legacy_gx_panic("GXSetTevKColor");
}

void GXSetTevKColorSel(GXTevStageID id, GXTevKColorSel sel) {
  aurora_legacy_gx_panic("GXSetTevKColorSel");
}

void GXSetTevKAlphaSel(GXTevStageID id, GXTevKAlphaSel sel) {
  aurora_legacy_gx_panic("GXSetTevKAlphaSel");
}

void GXSetTevSwapMode(GXTevStageID stageId, GXTevSwapSel rasSel, GXTevSwapSel texSel) {
  aurora_legacy_gx_panic("GXSetTevSwapMode");
}

void GXSetTevSwapModeTable(GXTevSwapSel id, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue,
                           GXTevColorChan alpha) {
  aurora_legacy_gx_panic("GXSetTevSwapModeTable");
}
}
