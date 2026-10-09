#pragma once
// The launcher's screen on the native renderer (docs/vita-launcher.md).
// It runs the same way as the game: the calling (game) thread records frame
// packets and the render thread owns the graphics context and presents.
#include <cstdint>
#include <functional>
#include <vector>

namespace VitaLauncher {
constexpr uint32_t ScreenWidth = 960, ScreenHeight = 544;

// A filled rectangle in screen pixels, origin top left; color is 0xAABBGGRR.
struct Rectangle {
    float x, y, w, h;
    uint32_t color;
};

// Creates the renderer at the screen size and starts its render thread. No
// game texture or model assets are opened: the disc may not be ready yet.
void StartScreen();

// One frame: the rectangles in order, or only the black clear when null.
// Never waits for the GPU; a frame the renderer cannot take is skipped.
void PresentScreen(const std::vector<Rectangle>* rectangles);

// Frames presented so far (the game's logical frame count starts after them).
uint64_t ScreenFrames();

// Runs task on the render thread and returns once it ran, presenting
// background (or black frames) meanwhile. A failing task throws here.
void RunOnRenderThread(std::function<void()> task, const std::vector<Rectangle>* background = nullptr);

// Ends the launcher's use of the renderer: stops the render thread, which
// hands the graphics context back to the calling thread, and opens the game's
// native assets (MH3_DISC_DIR). The last frame stays on the display until the
// game's own native graphics startup restarts the render thread.
void HandOffScreen();
#if defined(MH3_DEVELOPMENT) && !defined(__vita__)
// --launcher-screen-check: draws launcher frames, hands off and restarts the
// renderer the way the game does, and checks pixels and the target size.
int RunLauncherScreenCheck();
#endif
} // namespace VitaLauncher
