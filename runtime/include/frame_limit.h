#pragma once

#include <cstdint>

// The game thread limit: 60 (the default) or 30 frames a second, or 0 (off).
// With a limit the render thread consumes a packet once it submitted its frame
// to the GPU, waiting for a free GPU frame slot rather than skipping the draw,
// and the game thread records packet s only after packet s - 2 was consumed,
// so it runs at the rate frames are shown (PaceNativeFramePacket,
// docs/optimizations/threading/render-thread.md). On the Vita the display
// shows each frame for one VBlank at 60 and two at 30; GL has no VSync, so
// there both run at the rate the GPU finishes frames. Off, it waits only for
// a free packet slot and the render thread skips to the newest packet. A
// launcher sets it before the game starts: the game's render thread start
// applies it (or MH3_FRAME_LIMIT: 60, 30 or off).
uint32_t FrameLimitSetting();
void SetFrameLimitSetting(uint32_t fps);

// The VBlanks the limit shows each frame for: 1 at 60, 2 at 30, 0 off.
uint32_t FrameLimitVBlanks();
