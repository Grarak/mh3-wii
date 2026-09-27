#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <thread>

int RunGraphicsCheck(int argc, char** argv) {
    std::filesystem::create_directories("saves/graphics/cache");
    const auto user = std::filesystem::absolute("saves/graphics").string();
    const auto cache = std::filesystem::absolute("saves/graphics/cache").string();
    AuroraConfig config{};
    config.appName = "MH3 Metal renderer check";
    config.userPath = user.c_str();
    config.cachePath = cache.c_str();
    config.desiredBackend = BACKEND_METAL;
    config.windowWidth = 640;
    config.windowHeight = 480;
    config.msaa = 1;
    config.maxTextureAnisotropy = 1;
    const auto info = aurora_initialize(argc, argv, &config);
    if (info.initializationStatus != AURORA_INITIALIZATION_SUCCESS)
        throw std::runtime_error(info.initializationError ? info.initializationError : "Aurora initialization failed");
    struct Shutdown { ~Shutdown() { aurora_shutdown(); } } shutdown;
    if (info.backend != BACKEND_METAL) throw std::runtime_error("Aurora did not select Metal");
    unsigned frames = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (frames < 30 && std::chrono::steady_clock::now() < deadline) {
        for (const auto* event = aurora_update(); event && event->type != AURORA_NONE; ++event)
            if (event->type == AURORA_EXIT) throw std::runtime_error("Renderer check closed before completion");
        if (aurora_begin_frame()) {
            GXSetCopyClear(GXColor{20, 40, 80, 255}, GX_MAX_Z24);
            aurora_end_frame();
            ++frames;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    if (frames != 30) throw std::runtime_error("Metal frame check timed out");
    std::fprintf(stderr, "MH3 Metal renderer check: %u frames submitted\n", frames);
    return 0;
}
