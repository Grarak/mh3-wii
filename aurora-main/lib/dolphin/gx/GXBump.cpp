#include <aurora/legacy_gx.h>
#include "gx.hpp"
#include "__gx.h"

extern "C" {
void GXSetNumIndStages(u8 num) {
  aurora_legacy_gx_panic("GXSetNumIndStages");
}

void GXSetIndTexOrder(GXIndTexStageID indStage, GXTexCoordID texCoord, GXTexMapID texMap) {
  aurora_legacy_gx_panic("GXSetIndTexOrder");
}

void GXSetIndTexCoordScale(GXIndTexStageID indStage, GXIndTexScale scaleS, GXIndTexScale scaleT) {
  aurora_legacy_gx_panic("GXSetIndTexCoordScale");
}

void GXSetIndTexMtx(GXIndTexMtxID id, const void* offset, s8 scaleExp) {
  aurora_legacy_gx_panic("GXSetIndTexMtx");
}

void GXSetTevIndirect(GXTevStageID tevStage, GXIndTexStageID indStage, GXIndTexFormat fmt, GXIndTexBiasSel biasSel,
                      GXIndTexMtxID matrixSel, GXIndTexWrap wrapS, GXIndTexWrap wrapT, GXBool addPrev, GXBool indLod,
                      GXIndTexAlphaSel alphaSel) {
  aurora_legacy_gx_panic("GXSetTevIndirect");
}

void GXSetTevDirect(GXTevStageID stageId) {
  aurora_legacy_gx_panic("GXSetTevDirect");
}

void GXSetTevIndWarp(GXTevStageID tevStage, GXIndTexStageID indStage, GXBool signedOffsets, GXBool replaceMode,
                     GXIndTexMtxID matrixSel) {
  aurora_legacy_gx_panic("GXSetTevIndWarp");
}

void GXSetTevIndRepeat(GXTevStageID tevStage) {
  aurora_legacy_gx_panic("GXSetTevIndRepeat");
}

void GXSetTevIndBumpXYZ(GXTevStageID tevStage, GXIndTexStageID indStage, GXIndTexMtxID matrixSel) {
  aurora_legacy_gx_panic("GXSetTevIndBumpXYZ");
}

void GXSetTevIndBumpST(GXTevStageID tevStage, GXIndTexStageID indStage, GXIndTexMtxID matrixSel) {
  aurora_legacy_gx_panic("GXSetTevIndBumpST");
}

void GXSetTevIndTile(GXTevStageID tevStage, GXIndTexStageID indStage, u16 tileSizeS, u16 tileSizeT, u16 tileSizeS_exp,
                     u16 tileSizeT_exp, GXIndTexFormat fmt, GXIndTexMtxID matrixSel, GXIndTexBiasSel biasSel,
                     GXIndTexAlphaSel alphaSel) {
  aurora_legacy_gx_panic("GXSetTevIndTile");
}
}
