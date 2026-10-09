// Loading-screen progress (docs/loading-progress.md): what the loads behind
// a "Now Loading" screen have finished against what they will load, for the
// screen's progress bar (native_system_ui.cpp, HUDDraw).
#pragma once

#include <cstdint>

namespace LoadProgress {

// Game thread, at a disc read's submission: [offset, offset + length) of the
// DVD request ending at `end` (0: not known).
void DiscRead(uint64_t offset, uint32_t length, uint64_t end);

// Game thread: the read of [offset, offset + length) reached guest memory.
void DiscDelivered(uint64_t offset, uint32_t length);

// Asset cache (any thread): conversion work on `bytes` of a source file was
// queued, or finished (converted or failed).
void ConversionQueued(uint64_t bytes);
void ConversionDone(uint64_t bytes);

// Game thread, once per loading-screen frame: the share of the screen's
// loads done, 0..1. It never goes back within a screen and is 1 once
// nothing is left to load.
float Frame();

// Game thread, each frame without a loading screen: closes the last one.
void Idle();

} // namespace LoadProgress
