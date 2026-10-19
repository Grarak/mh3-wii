#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The game thread limit (PaceNativeFramePacket,
// docs/optimizations/threading/render-thread.md):
//
//   VSync (the default)  a frame for each display refresh: the render thread
//                        consumes a packet once it submitted its frame to the
//                        GPU, waiting for a free GPU frame slot rather than
//                        skipping the draw, and the game thread records packet
//                        s only after packet s - 2 was consumed, so it runs at
//                        the rate frames are shown. The Vita shows each for a
//                        VBlank (60 Hz), the web for one of the browser's
//                        animation frames (the display's own rate); GL has no
//                        VSync, so there it runs at the rate the GPU finishes
//                        frames.
//   HalfVSync            the same with each frame shown for two refreshes
//                        (30 Hz on the Vita).
//   Cap                  at most `fps` frames a second, timed by the game
//                        thread; the render thread shows the newest packet,
//                        skipping older ones.
//   Off                  the game thread waits only for a free packet slot,
//                        and the render thread shows the newest packet.
//
// A launcher sets it before the game starts: the game's render thread start
// applies it (or MH3_FRAME_LIMIT, ParseFrameLimit's form).
enum class FrameLimitMode : uint8_t { VSync, HalfVSync, Cap, Off };

struct FrameLimit {
    FrameLimitMode mode = FrameLimitMode::VSync;
    uint32_t fps = 0; // Cap's frames a second

    bool operator==(const FrameLimit&) const = default;
};

// A cap's range of frames a second.
constexpr uint32_t MinFrameCap = 10, MaxFrameCap = 1000;

FrameLimit FrameLimitSetting();
void SetFrameLimitSetting(FrameLimit limit);

// The VBlanks the limit shows each frame for: 1 with VSync, 2 with half
// VSync, else 0.
uint32_t FrameLimitVBlanks();

// The settings' form: vsync, half, off or cap:N; also 60 (vsync) and 30
// (half), as the Vita's launcher saved them before the caps. Nothing for
// anything else, a cap outside MinFrameCap..MaxFrameCap included.
std::optional<FrameLimit> ParseFrameLimit(std::string_view text);
std::string FrameLimitText(FrameLimit limit);
