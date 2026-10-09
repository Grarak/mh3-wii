#pragma once
#include "abi_bridge.h"
/* Thin guest entry points shared with the pre-existing registrations. */
void NativeEffectDrawBillboards(CpuContext* cpu);
uint32_t NativeEffectTextureSetup(CpuContext* cpu);
void NativeMovieFrameDecoded();

// Render thread: the copy of the movie planes at guest `address` under
// `generation` that the packet being drawn recorded, or nullptr.
const uint8_t* NativeMovieFrameCopy(uint32_t address, uint64_t generation, size_t bytes);

// Game thread, at a packet's begin: once no packet in the ring binds a movie
// frame, frees the planes' copy in the slot being recorded (none reads it)
// and retires the movie's frame texture.
void ReleaseMovieFrameCopy();
