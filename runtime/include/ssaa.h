#pragma once

#include <cstdint>

// Supersampling of the EFB in the web build, whose WebGPU renderer has no
// multisampling: 1 (off, the default), 2 or 4 times the pixels (1.41 or 2
// times the width and height), which presentation filters down into the
// canvas. A launcher sets it before the game starts: the game's
// InitializeNativeGraphics applies it. The development MH3_SSAA overrides it.
uint32_t SsaaSetting();
void SetSsaaSetting(uint32_t samples);
