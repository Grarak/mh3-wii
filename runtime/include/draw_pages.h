// MH3_TRACE=draw-pages (development, Linux hosts): which guest RAM a frame's
// draw touches that the next frame's logic writes, the conflicts a draw
// running beside the next logic step would have (draw_pages.cpp).
#pragma once

#include <cstdint>

namespace DrawPages {

// The logic phases the frame body runs, for when a conflicting write came.
enum class Phase : uint8_t {
    Input,
    Scene,
    Tasks,
    Timers,
};

#if defined(MH3_DEVELOPMENT) && !defined(__vita__)
bool Enabled();

// Logic of the next frame starts (its draw's pages were recorded).
void LogicPhase(Phase phase);

// The draw starts; stack is the game thread's guest stack pointer (its pages
// are the thread's own, not shared state).
void DrawBegins(uint32_t stack);

void DrawEnds();
#else
inline bool Enabled() {
    return false;
}

inline void LogicPhase(Phase) {}

inline void DrawBegins(uint32_t) {}

inline void DrawEnds() {}
#endif

} // namespace DrawPages
