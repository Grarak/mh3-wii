#pragma once
#include <cstdint>

// Create the renderer on the game thread. Native targets use only the selected
// visible dimensions, with no guest FIFO or XFB allocation/initialization.
void InitializeNativeGraphics(uint32_t width, uint32_t height);

// The game thread owns both frame admission and submission. Begin polls GPU
// completion once, before draw preparation or CDRAM writes. Always end the
// logical frame, even when drawing is skipped; neither boundary waits.
bool BeginNativeGraphicsFrame();
bool NativeGraphicsFrameShouldRender();
void EndNativeGraphicsFrame();
