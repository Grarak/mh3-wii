#pragma once
#include "abi_bridge.h"
/* Thin guest entry points shared with the pre-existing registrations. */
void NativeEffectDrawBillboards(CpuContext* cpu);
uint32_t NativeEffectTextureSetup(CpuContext* cpu);
void NativeMovieFrameDecoded();
