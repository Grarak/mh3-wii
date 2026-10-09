#pragma once
#include "abi_bridge.h"
/* Thin guest entry points shared with the pre-existing registrations. */
void NativeEffectDrawBillboards(CpuContext* cpu);
uint32_t NativeEffectTextureSetup(CpuContext* cpu);
void NativeMovieFrameDecoded();

// Render thread: the copy of the movie planes at guest `address` under
// `generation` that the packet being drawn recorded, or nullptr.
const uint8_t* NativeMovieFrameCopy(uint32_t address, uint64_t generation, size_t bytes);
