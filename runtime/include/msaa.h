#pragma once

#include <cstdint>

// Multisampling of the EFB: 1 (off, the default), 2 or 4 samples per pixel.
// The Vita's GXM renderer resolves it into the EFB's own pixels (the GL
// renderer too, where the driver offers that many samples). A launcher sets
// it before the game starts: the game's InitializeNativeGraphics applies it.
// The development MH3_MSAA overrides it.
uint32_t MsaaSetting();
void SetMsaaSetting(uint32_t samples);
