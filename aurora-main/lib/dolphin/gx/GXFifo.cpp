#include <aurora/legacy_gx.h>
#include "gx.hpp"

extern "C" {

static GXFifoObj* GPFifo;
static GXFifoObj* CPUFifo;

void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle, GXBool* cmdIdle, GXBool* brkpt) {
  aurora_legacy_gx_panic("GXGetGPStatus");
}

// TODO GXGetFifoStatus

void GXGetFifoPtrs(GXFifoObj* fifo, void** readPtr, void** writePtr) {
  aurora_legacy_gx_panic("GXGetFifoPtrs");
}

GXFifoObj* GXGetCPUFifo() {
  aurora_legacy_gx_panic("GXGetCPUFifo");
}

GXFifoObj* GXGetGPFifo() {
  aurora_legacy_gx_panic("GXGetGPFifo");
}

// TODO GXGetFifoBase
// TODO GXGetFifoSize
// TODO GXGetFifoLimits
// TODO GXSetBreakPtCallback
// TODO GXEnableBreakPt
// TODO GXDisableBreakPt

void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size) {
  aurora_legacy_gx_panic("GXInitFifoBase");
}

void GXInitFifoPtrs(GXFifoObj* fifo, void* readPtr, void* writePtr) {
  aurora_legacy_gx_panic("GXInitFifoPtrs");
}

// TODO GXInitFifoLimits

void GXSetCPUFifo(GXFifoObj* fifo) {
  aurora_legacy_gx_panic("GXSetCPUFifo");
}

void GXSetGPFifo(GXFifoObj* fifo) {
  aurora_legacy_gx_panic("GXSetGPFifo");
}

void GXSaveCPUFifo(GXFifoObj* fifo) {
  aurora_legacy_gx_panic("GXSaveCPUFifo");
}

// TODO GXSaveGPFifo
// TODO GXRedirectWriteGatherPipe
// TODO GXRestoreWriteGatherPipe
// TODO GXSetCurrentGXThread
// TODO GXGetCurrentGXThread
// TODO GXGetOverflowCount
// TODO GXResetOverflowCount
}