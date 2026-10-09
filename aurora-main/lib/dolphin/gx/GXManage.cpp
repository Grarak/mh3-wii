#include <aurora/legacy_gx.h>
#include "gx.hpp"
#include "__gx.h"

#include "../../gx/fifo.hpp"

#include <cstring>

// Global shadow register instance
static __GXData_struct sGXData;
__GXData_struct* __gx = &sGXData;
static GXFifoObj sFifoObj;

extern "C" {
static GXDrawDoneCallback DrawDoneCB = nullptr;

GXFifoObj* GXInit(void* base, u32 size) {
  aurora_legacy_gx_panic("GXInit");
}

void GXDrawDone() {
  aurora_legacy_gx_panic("GXDrawDone");
}

void GXSetDrawDone() {
  aurora_legacy_gx_panic("GXSetDrawDone");
}

GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb) {
  aurora_legacy_gx_panic("GXSetDrawDoneCallback");
}

void GXFlush() {
  aurora_legacy_gx_panic("GXFlush");
}

void GXPixModeSync() {
  aurora_legacy_gx_panic("GXPixModeSync");
}

void GXTexModeSync() {
  aurora_legacy_gx_panic("GXTexModeSync");
}

// Dirty state flush functions

void __GXSetDirtyState() {
  aurora_legacy_gx_panic("__GXSetDirtyState");
}

void __GXSetGenMode() {
  aurora_legacy_gx_panic("__GXSetGenMode");
}

void __GXSendFlushPrim() {
  aurora_legacy_gx_panic("__GXSendFlushPrim");
}

void __GXSetVCD() {
  aurora_legacy_gx_panic("__GXSetVCD");
}

void __GXSetVAT() {
  aurora_legacy_gx_panic("__GXSetVAT");
}

static void __SetSURegs(u32 tmap, u32 tcoord) {
  // Copy texture dimensions from tImage0 to SU registers
  u32 w = GET_REG_FIELD(__gx->tImage0[tmap], 10, 0);
  u32 h = GET_REG_FIELD(__gx->tImage0[tmap], 10, 10);
  SET_REG_FIELD(0, __gx->suTs0[tcoord], 16, 0, w);
  SET_REG_FIELD(0, __gx->suTs1[tcoord], 16, 0, h);

  // Bias from wrap mode
  u8 s_bias = GET_REG_FIELD(__gx->tMode0[tmap], 2, 0) == 1;
  u8 t_bias = GET_REG_FIELD(__gx->tMode0[tmap], 2, 2) == 1;
  SET_REG_FIELD(0, __gx->suTs0[tcoord], 1, 16, s_bias);
  SET_REG_FIELD(0, __gx->suTs1[tcoord], 1, 16, t_bias);
  GX_WRITE_RAS_REG(__gx->suTs0[tcoord]);
  GX_WRITE_RAS_REG(__gx->suTs1[tcoord]);
  __gx->bpSent = 1;
}

void __GXSetSUTexRegs() {
  aurora_legacy_gx_panic("__GXSetSUTexRegs");
}

void __GXUpdateBPMask() {
  aurora_legacy_gx_panic("__GXUpdateBPMask");
}

void __GXSetMatrixIndex(GXAttr matIdxAttr) {
  aurora_legacy_gx_panic("__GXSetMatrixIndex");
}
};
