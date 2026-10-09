#pragma once

// The game thread limit, on by default: the game thread records a frame only
// after the render thread took the one before it, so it runs at the rate
// frames are shown (PaceNativeFramePacket, docs/frame-packet-ring.md). Off,
// it waits only for a free packet slot and the render thread skips to the
// newest packet. A launcher sets it before the game starts: the game's render
// thread start applies it.
bool FrameLimitSetting();
void SetFrameLimitSetting(bool limit);
