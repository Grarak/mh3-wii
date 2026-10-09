#pragma once

// The game thread limit, on by default: the render thread consumes a packet
// once it submitted its frame to the GPU, waiting for a free GPU frame slot
// rather than skipping the draw, and the game thread records packet s only
// after packet s - 2 was consumed, so it runs at the rate frames are shown
// (PaceNativeFramePacket, docs/optimizations/threading/render-thread.md). Off,
// it waits only for a free packet slot and the render thread skips to the
// newest packet. A launcher sets it before the game starts: the game's render
// thread start applies it.
bool FrameLimitSetting();
void SetFrameLimitSetting(bool limit);
