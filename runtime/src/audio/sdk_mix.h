#pragma once

// RVL SDK MIX (sdk_mix.cpp) for native callers: the same functions as the
// registered entry points, without the guest calling convention. Levels are
// dB x 10, pans 0..127; `voice` is the AXVPB.
#include <cstdint>

namespace SdkMix {
void InitChannel(uint32_t voice, uint32_t mode, int32_t input, uint32_t auxA, uint32_t auxB, uint32_t auxC, uint32_t pan, uint32_t span, uint32_t fader);
void ReleaseChannel(uint32_t voice);
void SetInput(uint32_t voice, int32_t db);
void SetPan(uint32_t voice, int32_t pan);
void SetSPan(uint32_t voice, int32_t span);
void UpdateSettings();
uint32_t SoundMode();
} // namespace SdkMix
