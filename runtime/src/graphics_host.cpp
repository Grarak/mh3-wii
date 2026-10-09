#include "gx_host.h"
#include "memory.h"
#include "gx_stream.h"
#include "efb_host.h"
#include "webgpu/gpu.hpp"
#include "dolphin/vi/vi_internal.hpp"
#include <algorithm>
#include <atomic>
#include <vector>
#include <cstring>
#include <aurora/aurora.h>
#include <cstdlib>
#include <aurora/event.h>
#include <dolphin/gx.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <memory>
#include <webgpu/webgpu_cpp.h>
bool mh3_aurora_flush_draws();
namespace aurora::webgpu { extern wgpu::Queue g_queue; extern wgpu::Instance g_instance; }
namespace aurora::gfx { uint32_t current_frame() noexcept; void preserve_next_efb_frame(); }

namespace aurora::gx::fifo {
void init();
uint32_t process(const uint8_t*, uint32_t, bool);
}
namespace {
bool initialized = false;
bool frameActive = false;
bool gpuSynchronized = true, displayCopyPending = false, hasBegunFrame = false;
uint32_t captureInterval = 0;
std::filesystem::path capturePath;
GuestGxStream guestStream;
std::vector<uint8_t> efbPixels;
uint32_t efbPitch = 0, efbWidth = 0, efbHeight = 0;
bool efbBgra = false;
bool efbDirty = false;
uint32_t efbMinX = 0, efbMinY = 0, efbMaxX = 0, efbMaxY = 0;
void FlushHostEfbPokes() {
    if (!efbDirty) return;
    using namespace aurora::webgpu;
    if (!gpuSynchronized || g_graphicsConfig.msaaSamples != 1)
        throw std::runtime_error("EFB pokes require a completed single-sample framebuffer");
    aurora_wait_for_frame_worker();
    const wgpu::TexelCopyTextureInfo to{.texture = g_frameBuffer.texture, .origin = {efbMinX, efbMinY, 0}};
    const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = efbPitch, .rowsPerImage = efbMaxY - efbMinY};
    const wgpu::Extent3D extent{efbMaxX - efbMinX, efbMaxY - efbMinY, 1};
    const size_t offset = size_t(efbMinY) * efbPitch + efbMinX * 4;
    g_queue.WriteTexture(&to, efbPixels.data() + offset, efbPixels.size() - offset, &layout, &extent);
    // GXDrawDone is a synchronization point, not a Wii EFB clear. The next
    // host frame must retain the CPU's pixels for subsequent GXCopyTex/draws.
    if (!frameActive) aurora::gfx::preserve_next_efb_frame();
    efbDirty = false;
}
uint64_t guestCommands = 0, guestArrays = 0, guestTextures = 0, guestCopies = 0, guestDraws = 0, guestTextureCopies = 0;
void ShutdownGraphics() {
    if (initialized) {
        if (guestCommands) std::fprintf(stderr, "MH3 GX bridge: %llu commands consumed, %llu array conversions, %llu texture bindings, %llu display copies, %llu draws, %llu texture copies\n",
            static_cast<unsigned long long>(guestCommands), static_cast<unsigned long long>(guestArrays), static_cast<unsigned long long>(guestTextures), static_cast<unsigned long long>(guestCopies),
            static_cast<unsigned long long>(guestDraws), static_cast<unsigned long long>(guestTextureCopies));
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
    if (const char* path = std::getenv("MH3_CAPTURE_PATH")) {
        if (const char* interval = std::getenv("MH3_CAPTURE_INTERVAL")) {
            const auto value = std::stoul(interval);
            if (!value || value >= UINT32_MAX)
                throw std::runtime_error("MH3_CAPTURE_INTERVAL must be between 1 and 4294967294");
            captureInterval = static_cast<uint32_t>(value);
            capturePath = path;
        } else {
            const char* frame = std::getenv("MH3_CAPTURE_FRAME");
            aurora_request_frame_capture(frame ? std::stoul(frame) : 30, path);
        }
    }
}
}

void SubmitGuestGxBytes(const uint8_t* data, size_t size) {
    FlushHostEfbPokes();
    efbPixels.clear();
    gpuSynchronized = false;
    char name[] = "MH3";
    char* argv[]{name, nullptr};
    InitializeGraphics(1, argv);
    if (!frameActive) {
        aurora_update();
        if (hasBegunFrame) aurora::gfx::preserve_next_efb_frame();
        frameActive = aurora_begin_frame();
        hasBegunFrame = true;
        if (!frameActive) throw std::runtime_error("Aurora could not begin the guest GX frame");
    }
    guestStream.Write(std::span<const uint8_t>(data, size), [](auto packet) {
        const auto consumed = aurora::gx::fifo::process(packet.data(), packet.size(), true);
        if (consumed != packet.size()) throw std::runtime_error("Aurora did not consume the complete GX command");
        ++guestCommands;
        if (packet[0] >= 0x80 && packet[0] <= 0xBF) ++guestDraws;
        if (packet[0] == 0x50 && packet[1] == 0 && packet[2] >= 0x10 && packet[2] <= 0x1F) ++guestArrays;
        if (packet[0] == 0x50 && packet[1] == 0 && packet[2] == 0x30) ++guestTextures;
    }, [](uint32_t address) -> GuestGxStream::ArrayView {
        const uint32_t physical = address & 0x3FFFFFFF;
        uint32_t size;
        if (physical < 0x01800000) size = 0x01800000 - physical;
        else if (physical >= 0x10000000 && physical < 0x14000000) size = 0x14000000 - physical;
        else throw std::runtime_error("GX vertex array does not point into guest RAM");
        return {reinterpret_cast<uintptr_t>(Memory::GetPointer(address, size)), size};
    }, [](const GuestGxStream::DisplayCopy& copy) {
        GXSetDispCopyDst(copy.width, copy.height);
        GXCopyDisp(reinterpret_cast<void*>(copy.destination), copy.clear ? GX_TRUE : GX_FALSE);
        ++guestCopies;
        displayCopyPending = true;
    }, [](const GuestGxStream::TextureCopy& copy) {
        GXSetTexCopySrc(copy.x, copy.y, copy.sourceWidth, copy.sourceHeight);
        GXSetTexCopyDst(copy.width, copy.height, static_cast<GXTexFmt>(copy.format), copy.halfScale ? GX_TRUE : GX_FALSE);
        auto* destination = reinterpret_cast<void*>(copy.destination);
        GXCopyTex(destination, copy.clear ? GX_TRUE : GX_FALSE);
        // Eager readback keeps all translated RAM reads coherent, including
        // inlined accesses. A later demand-paged bridge may defer this wait.
        if (!aurora_flush_efb_copy_to_ram(destination))
            throw std::runtime_error("GX texture-copy readback failed");
        ++guestTextureCopies;
    });
}

void ServiceHostGraphicsEvents() {
    if (!initialized) return;
    for (const auto* event = aurora_update(); event && event->type != AURORA_NONE; ++event)
        if (event->type == AURORA_EXIT) throw std::runtime_error("MH3 window closed");
}

void CompleteHostGraphics() {
    if (!initialized) return;
    FlushHostEfbPokes();
    if (guestStream.PendingBytes()) throw std::runtime_error("GXDrawDone encountered a partial FIFO command");
    if (frameActive && !displayCopyPending) {
        if (!mh3_aurora_flush_draws()) throw std::runtime_error("GXDrawDone could not resume the in-progress frame");
    } else if (frameActive) {
        // Aurora advances its frame index while sealing, before capture encoding.
        const auto frame = aurora::gfx::current_frame() + 1;
        if (captureInterval && frame % captureInterval == 0) {
            const auto path = capturePath.parent_path() /
                (capturePath.stem().string() + "-" + std::to_string(frame) + ".bmp");
            aurora_request_frame_capture(frame, path.c_str());
        }
        aurora_end_frame(); frameActive = false; displayCopyPending = false;
    }
    aurora_wait_for_frame_worker();
    // Wait for actual submitted GPU work, not just the renderer worker's CPU
    // submission. Shared state remains valid even if WaitAny times out.
    auto result = std::make_shared<wgpu::QueueWorkDoneStatus>(wgpu::QueueWorkDoneStatus::Error);
    auto future = aurora::webgpu::g_queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly,
        [result](wgpu::QueueWorkDoneStatus status, wgpu::StringView) { *result = status; });
    const auto wait = aurora::webgpu::g_instance.WaitAny(future, 10000000000ULL);
    if (wait != wgpu::WaitStatus::Success || *result != wgpu::QueueWorkDoneStatus::Success)
        throw std::runtime_error("GXDrawDone failed while waiting for Metal completion");
    gpuSynchronized = true;
    static unsigned completions = 0;
    if (++completions <= 3 || completions % 300 == 0)
        std::fprintf(stderr, "MH3 GX draw completion %u: Metal work finished\n", completions);
}

uint32_t ReadHostEfbColor(uint16_t x, uint16_t y) {
    using namespace aurora::webgpu;
    if (!initialized || !gpuSynchronized || guestStream.PendingBytes())
        throw std::runtime_error("EFB color peek requires a completed GXDrawDone boundary");
    aurora_wait_for_frame_worker();
    const auto& source = g_graphicsConfig.msaaSamples > 1 ? g_frameBufferResolved : g_frameBuffer;
    if (!source.texture) throw std::runtime_error("EFB color texture is unavailable");
    if (efbPixels.empty() || efbWidth != source.size.width || efbHeight != source.size.height) {
        efbWidth = source.size.width; efbHeight = source.size.height;
        efbBgra = source.format == wgpu::TextureFormat::BGRA8Unorm ||
                  source.format == wgpu::TextureFormat::BGRA8UnormSrgb;
        if (!efbBgra && source.format != wgpu::TextureFormat::RGBA8Unorm &&
            source.format != wgpu::TextureFormat::RGBA8UnormSrgb)
            throw std::runtime_error("Unsupported Metal EFB color texture format");
        efbPitch = (efbWidth * 4 + 255) & ~255u;
        const uint64_t bytes = uint64_t(efbPitch) * efbHeight;
        const wgpu::BufferDescriptor desc{.label = "MH3 CPU EFB color readback",
            .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead, .size = bytes};
        auto buffer = g_device.CreateBuffer(&desc);
        auto encoder = g_device.CreateCommandEncoder();
        const wgpu::TexelCopyTextureInfo from{.texture = source.texture};
        const wgpu::TexelCopyBufferInfo to{.layout = {.bytesPerRow = efbPitch, .rowsPerImage = efbHeight},
                                          .buffer = buffer};
        encoder.CopyTextureToBuffer(&from, &to, &source.size);
        auto command = encoder.Finish();
        g_queue.Submit(1, &command);
        auto status = std::make_shared<std::atomic<wgpu::MapAsyncStatus>>(wgpu::MapAsyncStatus::CallbackCancelled);
        auto future = buffer.MapAsync(wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::WaitAnyOnly,
            [status](wgpu::MapAsyncStatus value, wgpu::StringView) { status->store(value); });
        if (g_instance.WaitAny(future, 10000000000ULL) != wgpu::WaitStatus::Success ||
            status->load() != wgpu::MapAsyncStatus::Success)
            throw std::runtime_error("Metal EFB color readback failed");
        const auto* pixels = static_cast<const uint8_t*>(buffer.GetConstMappedRange(0, bytes));
        if (!pixels) throw std::runtime_error("Metal EFB color readback has no mapped data");
        efbPixels.assign(pixels, pixels + bytes);
        buffer.Unmap();
        static unsigned snapshots = 0;
        if (++snapshots <= 3) std::fprintf(stderr, "MH3 EFB color snapshot %u: %ux%u, format %u\n",
            snapshots, efbWidth, efbHeight, guestStream.PixelFormat());
    }
    auto [logicalWidth, logicalHeight] = aurora::vi::configured_fb_size();
    if (!logicalWidth || !logicalHeight) { logicalWidth = efbWidth; logicalHeight = efbHeight; }
    if (x >= logicalWidth || y >= logicalHeight) throw std::runtime_error("EFB peek outside configured framebuffer");
    const uint32_t sx = uint64_t(2 * x + 1) * efbWidth / (2 * logicalWidth);
    const uint32_t sy = uint64_t(2 * y + 1) * efbHeight / (2 * logicalHeight);
    const auto* pixel = efbPixels.data() + sy * efbPitch + sx * 4;
    const uint32_t color = (uint32_t(pixel[3]) << 24) | (uint32_t(pixel[efbBgra ? 2 : 0]) << 16) |
                           (uint32_t(pixel[1]) << 8) | pixel[efbBgra ? 0 : 2];
    uint32_t alphaRead = 0;
    if (!ReadGxRegister(0xCC001008, 2, alphaRead)) throw std::runtime_error("PE alpha-read register unavailable");
    return ConvertEfbArgb(color, guestStream.PixelFormat(), alphaRead & 3);
}

void WriteHostEfbColor(uint16_t x, uint16_t y, uint32_t value) {
    (void)ReadHostEfbColor(x, y); // Ensure a coherent snapshot for masks and later peeks.
    if (aurora::webgpu::g_graphicsConfig.msaaSamples != 1)
        throw std::runtime_error("EFB color pokes do not support MSAA");
    uint32_t blend = 0, alpha = 0, depth = 0, destinationAlpha = 0;
    ReadGxRegister(0xCC001002, 2, blend);
    ReadGxRegister(0xCC001006, 2, alpha);
    ReadGxRegister(0xCC001000, 2, depth);
    ReadGxRegister(0xCC001004, 2, destinationAlpha);
    static bool traced = false;
    if (!traced) {
        std::fprintf(stderr, "MH3 EFB poke state: blend=%04x alpha=%04x depth=%04x destinationAlpha=%04x\n",
                     blend, alpha, depth, destinationAlpha);
        traced = true;
    }
    if ((blend & 0x803) || ((depth & 1) && ((depth >> 1) & 7) != 7))
        throw std::runtime_error("Unsupported EFB poke blending/logic/depth comparison");
    const unsigned a = value >> 24, threshold = alpha & 255;
    const bool compares[]{false, a < threshold, a == threshold, a <= threshold,
                          a > threshold, a != threshold, a >= threshold, true};
    if (!compares[(alpha >> 8) & 7]) return;
    if (destinationAlpha & 256) value = (value & 0xFFFFFF) | ((destinationAlpha & 255) << 24);
    value = ConvertEfbArgb(value, guestStream.PixelFormat(), 2);
    auto [width, height] = aurora::vi::configured_fb_size();
    if (!width || !height) { width = efbWidth; height = efbHeight; }
    const uint32_t x0 = uint64_t(x) * efbWidth / width, x1 = uint64_t(x + 1) * efbWidth / width;
    const uint32_t y0 = uint64_t(y) * efbHeight / height, y1 = uint64_t(y + 1) * efbHeight / height;
    if (!(blend & 0x18)) return;
    for (uint32_t row = y0; row < y1; ++row) for (uint32_t col = x0; col < x1; ++col) {
        auto* pixel = efbPixels.data() + row * efbPitch + col * 4;
        if (blend & 8) {
            pixel[efbBgra ? 2 : 0] = (value >> 16) & 255;
            pixel[1] = (value >> 8) & 255;
            pixel[efbBgra ? 0 : 2] = value & 255;
        }
        if (blend & 16) pixel[3] = value >> 24;
    }
    if (!efbDirty) { efbMinX = x0; efbMinY = y0; efbMaxX = x1; efbMaxY = y1; }
    else {
        efbMinX = std::min(efbMinX, x0); efbMinY = std::min(efbMinY, y0);
        efbMaxX = std::max(efbMaxX, x1); efbMaxY = std::max(efbMaxY, y1);
    }
    efbDirty = true;
}

int RunEfbCheck(int argc, char** argv) {
    InitializeGraphics(argc, argv);
    struct Shutdown { ~Shutdown() { ShutdownGraphics(); } } shutdown;
    using namespace aurora::webgpu;
    const auto& target = g_frameBuffer;
    if (g_graphicsConfig.msaaSamples != 1) throw std::runtime_error("EFB check requires single sampling");
    const bool bgra = target.format == wgpu::TextureFormat::BGRA8Unorm ||
                      target.format == wgpu::TextureFormat::BGRA8UnormSrgb;
    for (unsigned pass = 0; pass < 2; ++pass) {
        const uint32_t expected = pass ? 0xFFABCDEF : 0xFF123456;
        uint8_t pixels[16];
        for (unsigned i = 0; i < 4; ++i) {
            pixels[i*4] = bgra ? expected & 255 : (expected >> 16) & 255;
            pixels[i*4+1] = (expected >> 8) & 255;
            pixels[i*4+2] = bgra ? (expected >> 16) & 255 : expected & 255;
            pixels[i*4+3] = 255;
        }
        const wgpu::TexelCopyTextureInfo to{.texture = target.texture};
        const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = 8, .rowsPerImage = 2};
        const wgpu::Extent3D extent{2, 2, 1};
        g_queue.WriteTexture(&to, pixels, sizeof(pixels), &layout, &extent);
        efbPixels.clear();
        WriteGxRegister(0xCC001008, 2, 1);
        for (auto address : {0x08000000u, 0x88000004u, 0xC8001004u}) {
            uint32_t value;
            if (!ReadEfbColor32(address, value) || value != expected)
                throw std::runtime_error("Metal EFB color/row/alias check failed");
        }
        WriteGxRegister(0xCC001008, 2, 0);
        uint32_t value;
        if (!ReadEfbColor32(0xC8000000, value) || value != (expected & 0xFFFFFF))
            throw std::runtime_error("Metal EFB alpha-read check failed");
    }
    WriteGxRegister(0xCC001008, 2, 1);
    WriteGxRegister(0xCC001002, 2, 0x18);
    WriteGxRegister(0xCC001006, 2, 0x700);
    WriteGxRegister(0xCC001000, 2, 0x1F);
    if (!WriteEfbColor32(0xC8001004, 0xFF336699) || ReadHostEfbColor(1, 1) != 0xFF336699)
        throw std::runtime_error("EFB poke/peek coherence failed");
    FlushHostEfbPokes();
    efbPixels.clear();
    if (ReadHostEfbColor(1, 1) != 0xFF336699 || ReadHostEfbColor(0, 0) != 0xFFABCDEF)
        throw std::runtime_error("Metal EFB dirty-rectangle upload failed");
    frameActive = aurora_begin_frame();
    if (!frameActive) throw std::runtime_error("EFB preservation check could not begin a frame");
    gpuSynchronized = false;
    const auto beforeFence = aurora::gfx::current_frame();
    CompleteHostGraphics();
    if (!frameActive || aurora::gfx::current_frame() != beforeFence)
        throw std::runtime_error("GXDrawDone incorrectly presented an intermediate pass");
    efbPixels.clear();
    if (ReadHostEfbColor(1, 1) != 0xFF336699)
        throw std::runtime_error("GXDrawDone lost the in-progress EFB");
    displayCopyPending = true; // Exercise the completed-frame branch of the fence.
    CompleteHostGraphics();
    if (frameActive || aurora::gfx::current_frame() != beforeFence + 1)
        throw std::runtime_error("Completed display frame was not presented");
    efbPixels.clear();
    if (ReadHostEfbColor(1, 1) != 0xFF336699)
        throw std::runtime_error("CPU EFB pixels were lost across a host frame boundary");
    std::puts("MH3 Metal EFB color readback/poke check passed");
    return 0;
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
