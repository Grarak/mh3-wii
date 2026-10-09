// The launcher's screen on the native renderer (launcher_screen.h).
#include "launcher_screen.h"
#include "native_frame_packet.h"
#include "native_graphics.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/native_geometry.h>
#include "gl/device.hpp"
#include "gl/gl.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>

namespace VitaLauncher {
namespace {
// Four 28-byte vertices per rectangle. The Vita renderer copies each frame's
// geometry into 1 MiB of per-frame vertex memory and cancels a frame that
// does not fit, so a frame keeps below that: 9000 rectangles are 0.96 MiB.
constexpr size_t RectangleBudget = 9000, ChunkRectangles = 3000;

void SubmitRectangles(const std::vector<Rectangle>& rectangles) {
    mh3_raster_state raster{};
    raster.depth_compare = 7;
    raster.color_write = raster.alpha_write = 1;
    SetNativeRaster(raster);
    size_t total = rectangles.size();

    if (total > RectangleBudget) {
        static bool reported = false;

        if (!reported)
            std::fprintf(stderr, "MH3 launcher: %u rectangles in one frame, drawing the first %u\n", unsigned(total), unsigned(RectangleBudget));

        reported = true;
        total = RectangleBudget;
    }

    for (size_t base = 0; base < total; base += ChunkRectangles) {
        const size_t count = std::min(ChunkRectangles, total - base);
        AuroraNativeGeometry geometry{};

        if (!AllocateNativeGeometry(uint32_t(count * 4), uint32_t(count * 6), geometry))
            return;

        for (size_t i = 0; i < count; ++i) {
            const auto& r = rectangles[base + i];

            for (unsigned corner = 0; corner < 4; ++corner) {
                auto& v = geometry.vertices[i * 4 + corner];

                // GL clip space, Y up.
                v.position[0] = (r.x + (corner & 1 ? r.w : 0)) * (2.f / ScreenWidth) - 1;
                v.position[1] = 1 - (r.y + (corner & 2 ? r.h : 0)) * (2.f / ScreenHeight);
                v.position[2] = 0;
                v.position[3] = 1;
                v.uv[0] = v.uv[1] = 0;

                for (unsigned channel = 0; channel < 4; ++channel)
                    v.color[channel] = uint8_t(r.color >> (8 * channel));
            }

            const uint16_t first = uint16_t(i * 4);
            const uint16_t indices[] = {first, uint16_t(first + 1), uint16_t(first + 2), uint16_t(first + 2), uint16_t(first + 1), uint16_t(first + 3)};
            std::copy(std::begin(indices), std::end(indices), geometry.indices + i * 6);
        }

        SubmitNativeGeometry(geometry, false);
    }
}
} // namespace

void StartScreen() {
    InitializeNativeGraphics(ScreenWidth, ScreenHeight, false);
}

// Game thread.
namespace {
uint64_t presented = 0;
}

void PresentScreen(const std::vector<Rectangle>* rectangles) {
    if (BeginNativeGraphicsFrame() && rectangles)
        SubmitRectangles(*rectangles);

    EndNativeGraphicsFrame();
    ++presented;
}

uint64_t ScreenFrames() {
    return presented;
}

void RunOnRenderThread(std::function<void()> task, const std::vector<Rectangle>* background) {
    // Tasks run before the draws of the next packet the render thread takes,
    // so keep publishing packets until this one ran.
    const auto done = std::make_shared<std::atomic<bool>>(false);
    QueueNativeGraphicsTask(NativeTaskOrder::BeforeDraws, [task = std::move(task), done] {
        task();
        done->store(true, std::memory_order_release);
    });

    while (!done->load(std::memory_order_acquire)) {
        PresentScreen(background);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

void HandOffScreen() {
    StopNativeRenderThread();
    OpenNativeAssets();
}

#if defined(MH3_DEVELOPMENT) && !defined(__vita__)
int RunLauncherScreenCheck() {
    constexpr uint32_t ink = 0xff28302c, paper = 0xffc8d7d8, gold = 0xff73b9de;
    const auto pump = [] {
        for (const auto* e = aurora_update(); e && e->type != AURORA_NONE; ++e)
            if (e->type == AURORA_EXIT)
                throw std::runtime_error("Window closed during the launcher screen check");
    };

    const auto pixel = [](uint32_t x, uint32_t y) {
        const auto& target = aurora::gpu::g_frameBuffer;
        uint8_t rgba[4]{};
        aurora::gl::read_pixels(target.texture->gl(), x, y, 1, 1, rgba);
        return uint32_t(rgba[0]) | uint32_t(rgba[1]) << 8 | uint32_t(rgba[2]) << 16 | uint32_t(rgba[3]) << 24;
    };

    const auto near = [](uint32_t a, uint32_t b) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            if (std::abs(int(a >> shift & 0xff) - int(b >> shift & 0xff)) > 2)
                return false;
        return true;
    };

    StartScreen();

    // A gold bar in the top-left quarter tells up from down.
    const std::vector<Rectangle> frame{{0, 0, 960, 544, ink}, {30, 30, 900, 475, paper}, {60, 60, 300, 40, gold}};
    bool owner = false;
    RunOnRenderThread([&] { owner = aurora::gl::on_render_thread(); }, &frame);

    for (unsigned i = 0; i < 20; ++i) {
        pump();
        PresentScreen(&frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    // The launcher's hand-off, without the game's assets: the context returns here.
    StopNativeRenderThread();

    if (!owner || !aurora::gl::on_render_thread())
        throw std::runtime_error("Launcher screen: the render thread did not own, then return, the context");

    const auto& target = aurora::gpu::g_frameBuffer;
    const uint32_t corner = pixel(5, 5), middle = pixel(480, 300), bar = pixel(100, 80), mirrored = pixel(100, 544 - 80);
    std::fprintf(stderr, "MH3 launcher screen: target %ux%u, corner %08x, paper %08x, bar %08x, mirrored bar %08x\n", target.width, target.height, corner, middle, bar, mirrored);

    if (target.width != ScreenWidth || target.height != ScreenHeight || !near(corner, ink) || !near(middle, paper) || !near(bar, gold) || !near(mirrored, paper))
        throw std::runtime_error("Launcher screen: rectangles were not drawn where expected");
    // The game's own native graphics startup at its render mode restarts the
    // render thread; its frames then draw at the new target size.
    InitializeNativeGraphics(640, 456);

    for (unsigned i = 0; i < 20; ++i) {
        pump();
        PresentScreen(nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    StopNativeRenderThread();
    const uint32_t cleared = pixel(100, 80);
    std::fprintf(stderr, "MH3 launcher screen: after the game's startup target %ux%u, pixel %08x\n", target.width, target.height, cleared);

    // Every host renders for the 960x544 display: the game's 640x456 render
    // mode fits it at its own aspect (764x544).
    if (target.height != ScreenHeight || std::abs(double(target.width) / target.height - 640.0 / 456) > 0.01 || !near(cleared, 0xff000000))
        throw std::runtime_error("Launcher screen: the restarted renderer did not take the game's target");

    std::fprintf(stderr, "MH3 launcher screen check passed\n");
    return 0;
}
#endif
} // namespace VitaLauncher
