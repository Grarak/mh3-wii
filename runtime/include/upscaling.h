#pragma once

// The EFB's resolution, upscaling on by default: the target is rendered at
// the display's resolution (960x544 in 16:9, 777x544 pillarboxed in 4:3).
// Off, it keeps the Wii's 448 rows at the same aspect (791x448, 640x448) and
// presentation scales it to the display. A launcher sets it before the game
// starts: the game's InitializeNativeGraphics applies it. The development
// MH3_RENDER_SCALE overrides it.
bool UpscalingSetting();
void SetUpscalingSetting(bool upscaling);
