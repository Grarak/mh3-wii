#include "gx_host.h"
#include "memory.h"
#include "gx_stream.h"
#include <aurora/aurora.h>
#include <cstdlib>
#include <aurora/event.h>
#include <dolphin/gx.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace aurora::gx::fifo {
void init();
uint32_t process(const uint8_t*, uint32_t, bool);
}
namespace {
bool initialized = false;
bool frameActive = false;
GuestGxStream guestStream;
uint64_t guestCommands = 0, guestArrays = 0;
void ShutdownGraphics() {
    if (initialized) {
        if (guestCommands) std::fprintf(stderr, "MH3 GX bridge: %llu commands consumed, %llu array conversions\n",
            static_cast<unsigned long long>(guestCommands), static_cast<unsigned long long>(guestArrays));
        aurora_shutdown(); initialized = false; frameActive = false;
    }
}
void InitializeGraphics(int argc, char** argv) {
    if (initialized) return;
    std::filesystem::create_directories("saves/graphics/cache");
    const auto user = std::filesystem::absolute("saves/graphics").string();
    const auto cache = std::filesystem::absolute("saves/graphics/cache").string();
    AuroraConfig config{};
    config.appName = "Monster Hunter Tri";
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
    initialized = true;
    std::atexit(ShutdownGraphics);
    if (info.backend != BACKEND_METAL) throw std::runtime_error("Aurora did not select Metal");
    aurora::gx::fifo::init();
}
}

void SubmitGuestGxBytes(const uint8_t* data, size_t size) {
    char name[] = "MH3";
    char* argv[]{name, nullptr};
    InitializeGraphics(1, argv);
    if (!frameActive) {
        aurora_update();
        frameActive = aurora_begin_frame();
        if (!frameActive) throw std::runtime_error("Aurora could not begin the guest GX frame");
    }
    guestStream.Write(std::span<const uint8_t>(data, size), [](auto packet) {
        const auto consumed = aurora::gx::fifo::process(packet.data(), packet.size(), true);
        if (consumed != packet.size()) throw std::runtime_error("Aurora did not consume the complete GX command");
        ++guestCommands;
        if (packet[0] == 0x50 && packet[1] == 0 && packet[2] >= 0x10 && packet[2] <= 0x1F) ++guestArrays;
    }, [](uint32_t address) -> GuestGxStream::ArrayView {
        const uint32_t physical = address & 0x3FFFFFFF;
        uint32_t size;
        if (physical < 0x01800000) size = 0x01800000 - physical;
        else if (physical >= 0x10000000 && physical < 0x14000000) size = 0x14000000 - physical;
        else throw std::runtime_error("GX vertex array does not point into guest RAM");
        return {reinterpret_cast<uintptr_t>(Memory::GetPointer(address, size)), size};
    });
}

int RunGraphicsCheck(int argc, char** argv) {
    InitializeGraphics(argc, argv);
    struct Shutdown { ~Shutdown() { ShutdownGraphics(); } } shutdown;
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
