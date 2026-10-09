#pragma once

// The game thread owns both frame admission and submission. Begin polls GPU
// completion once, before draw preparation or CDRAM writes. Always end the
// logical frame, even when drawing is skipped; neither boundary waits.
bool BeginNativeGraphicsFrame();
bool NativeGraphicsFrameShouldRender();
void EndNativeGraphicsFrame();
