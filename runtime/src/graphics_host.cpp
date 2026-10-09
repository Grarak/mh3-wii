#include "gx_host.h"
#include "gx_panic.h"
#include "native_graphics.h"
#include "native_frame_packet.h"
#include "asset_cache.h"
#include "model_assets.h"
#include "native_frame_statistics.h"
#include "audio/ax_dsp.h"
#include "audio_output_statistics.h"
#include "audio_dma_statistics.h"
#ifdef __vita__
#include "../vita/platform.h"
#include "gxm/razor.hpp"
#include "gxm/device.hpp"
#include "gxm/residency.hpp"
#endif
#include "memory.h"
#include "game_step.h"
#include "gx_stream.h"
#include "efb_host.h"
#include "isa/ppc_isa_fpenv.h"
#include "gl/device.hpp"
#include "gl/gl.hpp"
#include "dolphin/vi/vi_internal.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <vector>
#include <cstring>
#include <aurora/aurora.h>
#include <aurora/generated_texture.hpp>
#include <aurora/native_scene_feedback.hpp>
#include <aurora/native_draw_memory.hpp>
#include <aurora/native_geometry.h>
#include <aurora/thread_role.hpp>
#include <cstdlib>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <string>
#if defined(MH3_ENABLE_GX_DIAGNOSTICS) || defined(MH3_DEVELOPMENT)
#include "gx/gx.hpp"
#endif
#ifdef MH3_DEVELOPMENT
#include "gx/command_processor.hpp"
#include "isa/ppc_isa_context.h"
#endif
#ifdef MH3_ENABLE_GX_DIAGNOSTICS
#include <fstream>
#endif
#ifdef MH3_ENABLE_EFB_COPY_TRACE
#include "efb_copy_trace.h"
#endif
namespace aurora::gfx {
uint32_t current_frame() noexcept;
void preserve_next_efb_frame() noexcept;
void split_staging_batch();
void require_frame_render() noexcept;
bool frame_skipped() noexcept;
std::array<uint64_t, 4> staging_high_water() noexcept;
uint64_t staging_split_count() noexcept;
} // namespace aurora::gfx

namespace aurora::gx::fifo {
void init();
uint32_t process(const uint8_t*, uint32_t, bool);
} // namespace aurora::gx::fifo
namespace {
void BeginHostGraphicsFrame();
bool initialized = false;
bool frameActive = false;
bool gpuSynchronized = true, displayCopyPending = false, hasBegunFrame = false;
#ifdef __vita__
uint64_t razorNativeFrameStarted = 0;
#endif
uint32_t captureInterval = 0;
uint32_t captureStart = 0;
std::filesystem::path capturePath;
GuestGxStream guestStream;
std::vector<uint8_t> efbPixels;
uint32_t efbPitch = 0, efbWidth = 0, efbHeight = 0;
bool efbBgra = false;
bool efbDirty = false;
uint32_t efbMinX = 0, efbMinY = 0, efbMaxX = 0, efbMaxY = 0;
// Event polling and all graphics submission belong to the game thread.

void FlushHostEfbPokes() {
    if (!efbDirty)
        return;
    if (!gpuSynchronized)
        throw std::runtime_error("EFB pokes require a completed framebuffer");
    // Queued on the render thread behind every earlier draw, ahead of later ones.
    const uint32_t width = efbMaxX - efbMinX, height = efbMaxY - efbMinY;
    std::vector<uint8_t> rect(size_t(width) * height * 4);
    for (uint32_t row = 0; row < height; ++row)
        std::memcpy(rect.data() + size_t(row) * width * 4, efbPixels.data() + size_t(efbMinY + row) * efbPitch + efbMinX * 4, size_t(width) * 4);
    aurora::gl::write_texture(aurora::gpu::g_frameBuffer.texture, 0, efbMinX, efbMinY, width, height, std::move(rect), width * 4);
    // GXDrawDone is a synchronization point, not a Wii EFB clear. The next
    // host frame must retain the CPU's pixels for subsequent GXCopyTex/draws.
    if (!frameActive)
        aurora::gfx::preserve_next_efb_frame();
    efbDirty = false;
}
std::atomic<uint64_t> guestCopies{0}; // logical frames, counted on the game thread
#ifdef MH3_ENABLE_GX_DIAGNOSTICS
#include "gx_diagnostics.inc"
#endif
// Stats (MH3_STATS line, MH3_PROFILE_FRAMES profile, GX counters) are built
// only with MH3_DEVELOPMENT; MH3_STAT(...) compiles its argument away otherwise.
#ifdef MH3_DEVELOPMENT
#define MH3_STAT(...) __VA_ARGS__
uint64_t guestArrays = 0, guestTextures = 0, guestTextureCopies = 0;
uint64_t guestEfbSnapshots = 0, guestEfbPeeks = 0;
uint64_t guestTextureCopyNs = 0, guestEfbSnapshotNs = 0;
struct FrameProfile {
    using Clock = std::chrono::steady_clock;
    uint32_t interval = 0;
    uint64_t renderQueueNs = 0, tailNs = 0, completionNs = 0;
    uint64_t minCopyNs = UINT64_MAX, maxCopyNs = 0, measuredCopies = 0;
    uint64_t lastCopies = 0, lastCommands = 0, lastArrays = 0, lastTextures = 0, lastDraws = 0;
    uint64_t lastSkippedDraws = 0;
    uint64_t lastTextureCopies = 0, lastEfbSnapshots = 0, lastEfbPeeks = 0;
    uint64_t lastTextureHashes = 0, lastTextureInvalidates = 0;
    uint64_t lastTextureCopyNs = 0, lastEfbSnapshotNs = 0;
    Clock::time_point started = Clock::now();
    Clock::time_point previousCopy{};
    std::clock_t cpuStarted = std::clock();
};
FrameProfile profile;
#else
#define MH3_STAT(...)
#endif
void ShutdownGraphics() {
    if (initialized) {
        // Drain the render thread; it hands the graphics context back.
        StopNativeRenderThread();
        MH3_STAT(if (GuestGxStream::commandCount)
                     std::fprintf(stderr, "MH3 GX bridge: %llu commands consumed, %llu array conversions, %llu texture bindings, %llu display copies, %llu draws, %llu texture copies\n",
                                  static_cast<unsigned long long>(GuestGxStream::commandCount), static_cast<unsigned long long>(guestArrays), static_cast<unsigned long long>(guestTextures),
                                  static_cast<unsigned long long>(guestCopies), static_cast<unsigned long long>(GuestGxStream::drawCount), static_cast<unsigned long long>(guestTextureCopies)));
        // Shader patcher storage must outlive the last submitted draw. This
        // drain is only at shutdown; frame admission/submission never waits.
        aurora::gl::finish();
        UnloadNativeModelAssets();
        ClearNativeMaterialPrograms();
        aurora::clear_scene_feedback_textures();
        aurora::clear_generated_textures();
        aurora_shutdown();
        initialized = false;
        frameActive = false;
    }
}
} // namespace
void OpenNativeAssets() {
    const char* discRoot = std::getenv("MH3_DISC_DIR");
    const char* nativeRoot = std::getenv("MH3_NATIVE_ASSET_DIR");
    const auto nativePack = std::filesystem::path(nativeRoot ? nativeRoot
                                                  : discRoot ? discRoot
                                                             :
#ifdef __vita__
                                                             "ux0:/data/mh3-wii"
#else
                                                             "Assets/DATA"
#endif
                                                  ) /
                            "native-textures.bin";
    if (std::filesystem::exists(nativePack))
        aurora::gl::open_native_texture_pack(nativePack.string().c_str());
    else if (GameAssetCache())
        // Load-time conversion: a texture's pack is opened at its first use.
        aurora::gl::set_native_texture_resolver([](uint32_t asset) {
            try {
                aurora::gl::open_native_texture_pack(GameAssetCache()->TexturePack(asset).c_str());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "MH3: %s\n", e.what());
                std::abort();
            }
        });
    LoadNativeModelAssets(nativeRoot ? nativeRoot : discRoot);
}
namespace {
void InitializeGraphics(int argc, char** argv, bool loadNativeAssets = true) {
    if (initialized)
        return;
    const char* userRoot = std::getenv("MH3_USER_DIR");
    const auto graphicsRoot = (userRoot ? std::filesystem::path(userRoot) : "saves") / "graphics";
    std::filesystem::create_directories(graphicsRoot / "cache");
#ifdef __vita__
    // libstdc++'s POSIX absolute() treats the Vita mount prefix as relative.
    const auto user = graphicsRoot.string();
    const auto cache = (graphicsRoot / "cache").string();
#else
    const auto user = std::filesystem::absolute(graphicsRoot).string();
    const auto cache = std::filesystem::absolute(graphicsRoot / "cache").string();
#endif
    AuroraConfig config{};
    config.appName = "Monster Hunter Tri";
    config.userPath = user.c_str();
    config.cachePath = cache.c_str();
#ifdef AURORA_GXM
    config.desiredBackend = BACKEND_GXM;
#else
    config.desiredBackend = BACKEND_OPENGLES;
#endif
    config.windowWidth = 640;
    config.windowHeight = 480;
#ifdef MH3_DEVELOPMENT
    // MH3_WINDOW_SCALE=n: an n times larger window. It only shows the picture
    // larger: pair it with MH3_RENDER_SCALE to render at that resolution.
    if (const char* v = std::getenv("MH3_WINDOW_SCALE")) {
        const int n = std::atoi(v);
        if (n > 1) {
            config.windowWidth *= n;
            config.windowHeight *= n;
        }
    }
#endif
    config.msaa = 1;
    config.maxTextureAnisotropy = 1;
    const auto info = aurora_initialize(argc, argv, &config);
    if (info.initializationStatus != AURORA_INITIALIZATION_SUCCESS)
        throw std::runtime_error(info.initializationError ? info.initializationError : "Aurora initialization failed");
    initialized = true;
    std::atexit(ShutdownGraphics);
    if (loadNativeAssets)
        OpenNativeAssets();
#ifdef MH3_DEVELOPMENT
    // MH3_RENDER_SCALE=n: render the EFB at n times the native resolution,
    // whatever the window size (captures are at that size).
    if (const char* v = std::getenv("MH3_RENDER_SCALE")) {
        const float n = std::strtof(v, nullptr);
        if (n > 0)
            VISetFrameBufferScale(n);
    }
    // The draw log looks at one command per packet.
    if (std::getenv("MH3_GX_DRAW_LOG"))
        guestStream.coalesce = false;
    if (std::getenv("MH3_GX_SOURCE_CAPTURE")) {
        guestStream.traceDraw = aurora::gx::fifo::set_capture_display_list;
    }
#endif
#ifdef MH3_ENABLE_GX_DIAGNOSTICS
    guestStream.coalesce = false;
#endif
#ifdef MH3_DEVELOPMENT
    if (const char* interval = std::getenv("MH3_PROFILE_FRAMES")) {
        const auto value = std::stoul(interval);
        if (!value || value > UINT32_MAX)
            throw std::runtime_error("MH3_PROFILE_FRAMES must be a positive 32-bit number");
        profile.interval = static_cast<uint32_t>(value);
        profile.started = FrameProfile::Clock::now();
        profile.cpuStarted = std::clock();
    }
#endif
    if (info.backend != config.desiredBackend)
        throw std::runtime_error("Aurora did not select the required renderer");
    // Frameskip polls GPU completion before native draw preparation.
    aurora_set_frameskip(true);
#ifdef MH3_DEVELOPMENT
    const char* frameskip = std::getenv("MH3_FRAMESKIP");
    if (frameskip && frameskip[0] == '0')
        aurora_set_frameskip(false);
    if (const char* path = std::getenv("MH3_CAPTURE_PATH")) {
        if (const char* interval = std::getenv("MH3_CAPTURE_INTERVAL")) {
            const auto value = std::stoul(interval);
            if (!value || value >= UINT32_MAX)
                throw std::runtime_error("MH3_CAPTURE_INTERVAL must be between 1 and 4294967294");
            captureInterval = static_cast<uint32_t>(value);
            capturePath = path;
            // MH3_CAPTURE_START=n: no captures before frame n.
            if (const char* start = std::getenv("MH3_CAPTURE_START"))
                captureStart = uint32_t(std::stoul(start));
        } else {
            const char* frame = std::getenv("MH3_CAPTURE_FRAME");
            aurora_request_frame_capture(frame ? std::stoul(frame) : 30, path);
        }
    }
#endif
}
} // namespace

void SealDisplayFrame() {
    // Aurora advances its frame index while sealing, before capture encoding.
    const auto frame = aurora::gfx::current_frame() + 1;
#ifdef MH3_DEVELOPMENT
    // MH3_STEP_LOG=file: "frame ticks scale" for the step each display frame shows.
    static FILE* stepLog = [] {
        const char* path = std::getenv("MH3_STEP_LOG");
        return path ? std::fopen(path, "w") : nullptr;
    }();
    // MH3_STEP_LOG_WORDS=addr[,addr...] (hex) adds those words (hex).
    static const std::vector<uint32_t> logWords = [] {
        std::vector<uint32_t> words;
        for (const char* v = std::getenv("MH3_STEP_LOG_WORDS"); v && *v;) {
            char* end = nullptr;
            words.push_back(uint32_t(std::strtoul(v, &end, 16)));
            if (end == v)
                break;
            v = *end == ',' ? end + 1 : end;
        }
        return words;
    }();
    if (stepLog) {
        std::fprintf(stepLog, "%llu %u %.4f", (unsigned long long)frame, GameStep::Ticks(), GameStep::Scale());
        for (uint32_t word : logWords)
            std::fprintf(stepLog, " %08x", Memory::Read32(word));
        std::fprintf(stepLog, "\n");
    }
#endif
    if (captureInterval && frame >= captureStart && frame % captureInterval == 0) {
        const auto path = capturePath.parent_path() / (capturePath.stem().string() + "-" + std::to_string(frame) + ".bmp");
        aurora_request_frame_capture(frame, path.c_str());
    }
    { aurora_end_frame(); }
    frameActive = false;
    displayCopyPending = false;
}

bool quitRequested = false;

// Reads the host events; a quit is kept for ServiceHostGraphicsEvents.
void PollHostGraphicsEvents() {
    for (const auto* event = aurora_update(); event && event->type != AURORA_NONE; ++event)
        if (event->type == AURORA_EXIT)
            quitRequested = true;
}

namespace {
void BeginHostGraphicsFrame() {
    if (frameActive)
        return;
    if (hasBegunFrame)
        aurora::gfx::preserve_next_efb_frame();
    frameActive = aurora_begin_frame();
    hasBegunFrame = true;
    if (!frameActive)
        throw std::runtime_error("Aurora could not begin the guest GX frame");
    const auto frame = aurora::gfx::current_frame() + 1;
    if (captureInterval && frame >= captureStart && frame % captureInterval == 0)
        aurora::gfx::require_frame_render();
}
} // namespace

#ifdef MH3_DEVELOPMENT
static std::vector<uint8_t>* gxCapture = nullptr;
void CaptureGuestGx(std::vector<uint8_t>* into) {
    gxCapture = into;
}
#endif

void SubmitGuestGxBytes(const uint8_t*, size_t) {
    PanicLegacyGx("GX FIFO write");
}

void SubmitHostGraphicsTask(std::function<void()> task) {
    char name[] = "MH3";
    char* argv[]{name, nullptr};
    InitializeGraphics(1, argv);
    // Runs on the render thread before the draws of the current frame.
    QueueNativeGraphicsTask(NativeTaskOrder::BeforeDraws, std::move(task));
}

void InitializeNativeGraphics(uint32_t width, uint32_t height, bool load_native_assets) {
    if (NativeRenderThreadRunning())
        throw std::runtime_error("Native graphics initialized twice");
    if (initialized && !aurora::gl::on_render_thread())
        throw std::runtime_error("Native graphics initialized outside the game thread");
    if (!aurora_configure_native_target(width, height))
        throw std::runtime_error("Native graphics target has zero dimensions");
    char name[] = "MH3";
    char* argv[]{name, nullptr};
    InitializeGraphics(1, argv, load_native_assets);
    if (!aurora::gl::on_render_thread())
        throw std::runtime_error("Native graphics initialized outside the game thread");
    // From here the render thread alone talks to the graphics API.
    StartNativeRenderThread();
}

bool BeginNativeGraphicsFrame() {
    if (!initialized)
        throw std::runtime_error("Native frame began before graphics startup");
    MH3_STAT(BeginNativeFrameStatistics();)
#ifdef __vita__
    // Include admission and logical work on skipped frames. Razor is opt-in;
    // this returns zero without a clock read when live CPU tracing is inactive.
    razorNativeFrameStarted = aurora::gxm::razor_begin_cpu_work();
#endif
    BeginNativeFramePacket(aurora_native_window_presentable());
    return NativeFramePacketRecording();
}

bool NativeGraphicsFrameShouldRender() {
    if (!initialized)
        return false;
    if (NativeRenderThreadRunning() && !aurora::gl::on_render_thread())
        return NativeFramePacketRecording();
    return aurora_native_frame_should_render();
}
void CancelNativeGraphicsFrame() {
    if (!initialized)
        throw std::runtime_error("Native frame canceled before graphics startup");
    if (NativeRenderThreadRunning() && !aurora::gl::on_render_thread())
        CancelNativeFramePacket();
    else
        aurora_cancel_native_frame();
}

void EndNativeGraphicsFrame() {
    MH3_STAT(const bool rendered = NativeFramePacketRecording();)
#ifdef __vita__
    // Close before presentation's heartbeat flushes/stops live collection.
    // This span excludes presentation and the subsequent frame-pacer sleep.
    aurora::gxm::razor_end_cpu_work(razorNativeFrameStarted);
    razorNativeFrameStarted = 0;
#endif
    std::string capture;
    const auto frame = NativeFramePacketSequence();
    if (captureInterval && frame >= captureStart && frame % captureInterval == 0)
        capture = (capturePath.parent_path() / (capturePath.stem().string() + "-" + std::to_string(frame) + ".bmp")).string();
    PublishNativeFramePacket(std::move(capture));
    MH3_STAT(EndNativeFrameStatistics(rendered);)
    ++guestCopies;
}

template <size_t N>
void SubmitGuestGxFixed(const uint8_t* data) {
    GX_HLE_FIFO_WriteBytesInline(data, N);
}
template void SubmitGuestGxFixed<1>(const uint8_t*);
template void SubmitGuestGxFixed<2>(const uint8_t*);
template void SubmitGuestGxFixed<4>(const uint8_t*);

#ifdef MH3_DEVELOPMENT
// The render thread owns the feedback pool; it copies the counters after a
// frame and the game thread reports the last copy.
aurora::SceneFeedbackStats ReadSceneFeedbackStats() {
    static std::mutex lock;
    static aurora::SceneFeedbackStats latest;
    if (!NativeRenderThreadRunning())
        return aurora::scene_feedback_stats();
    QueueNativeGraphicsTask(NativeTaskOrder::AfterDraws, [] {
        const auto stats = aurora::scene_feedback_stats();
        std::lock_guard guard(lock);
        latest = stats;
    });
    std::lock_guard guard(lock);
    return latest;
}
#endif
// Development stats default on; MH3_STATS=0 disables the once-per-second line.
// Game speed is retrace periods the frame pacer stepped (frames * budget)
// versus the nominal VI rate, since the game logic is fixed-step per frame;
// Logical FPS counts complete game steps. Submitted FPS counts presentation
// requests; on GXM a separate counter measures completed display flips.
void ReportHostSpeed([[maybe_unused]] double ticks, [[maybe_unused]] double nominalHz) {
#ifdef __vita__
    ReportVitaMemory();
#endif
#ifdef MH3_DEVELOPMENT
    static const bool enabled = [] {
        const char* value = std::getenv("MH3_STATS");
        return !value || std::strcmp(value, "0") != 0;
    }();
    if (!enabled || !initialized)
        return;
    using Clock = std::chrono::steady_clock;
    static auto last = Clock::now();
    static double lastTicks = ticks;
    static uint64_t lastCopies = guestCopies, lastPresents = aurora_presented_frame_count(), lastSkipped = aurora_skipped_frame_count();
    static auto lastWaits = aurora::gfx::staging_wait_counters();
    static auto previousAx = AxDspHle::GetMixWorkerStats();
    static auto previousAudio = GetNativeAudioOutputStatistics();
    static auto previousDma = GetNativeAudioDmaStatistics();
    static auto previousFeedback = ReadSceneFeedbackStats();
#ifdef AURORA_GXM
    static auto previousDisplay = aurora::gxm::display_statistics();
#endif
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - last).count();
    if (seconds < 1.0)
        return;
    const uint64_t presents = aurora_presented_frame_count(), skipped = aurora_skipped_frame_count();
    const double tickHz = (ticks - lastTicks) / seconds;
    std::fprintf(stderr,
                 "MH3 stats: game speed %.1f%% (%.1f/%.1f Hz paced), logical %.1f fps, submitted %.1f fps, "
                 "%.1f fps skipped, logical_total=%llu, NI %s\n",
                 100.0 * tickHz / nominalHz, tickHz, nominalHz, (guestCopies - lastCopies) / seconds, (presents - lastPresents) / seconds, (skipped - lastSkipped) / seconds,
                 static_cast<unsigned long long>(guestCopies.load()), g_mkwHostNiActive ? "set" : "clear");
    const auto work = TakeNativeFrameStatistics();
    std::fprintf(stderr, "MH3 native work: admitted render %.1f fps, frame %.3f ms avg/%.3f max, rendered %.3f ms avg/%.3f max (%llu/%llu frames)\n",
                 work.window_ns ? 1e9 * double(work.rendered_frames) / double(work.window_ns) : 0.0, work.measured_frames ? work.frame_wall_ns / (1e6 * work.measured_frames) : 0.0,
                 work.frame_wall_max_ns / 1e6, work.rendered_measured_frames ? work.rendered_wall_ns / (1e6 * work.rendered_measured_frames) : 0.0, work.rendered_wall_max_ns / 1e6,
                 static_cast<unsigned long long>(work.rendered_frames), static_cast<unsigned long long>(work.logical_frames));
    if (work.owner_running_available && work.window_ns)
        std::fprintf(stderr, "MH3 native work: owner CPU %.1f%%, thread %d core %d affinity %x (scheduled execution; window %.3f s)\n", 1e5 * double(work.owner_running_us) / double(work.window_ns),
                     work.owner_thread, work.owner_core, unsigned(work.owner_affinity), double(work.window_ns) / 1e9);
    else
        std::fprintf(stderr, "MH3 native work: owner CPU unavailable\n");
#ifdef AURORA_GXM
    const auto display = aurora::gxm::display_statistics();
    std::fprintf(stderr, "MH3 native work: displayed %.1f fps, backlog %u, scenes_pending %u, display_errors %u, callback wait %.2f ms/s (core0)\n",
                 uint32_t(display.presented - previousDisplay.presented) / seconds, display.backlog, display.pending_scenes, display.failures,
                 (display.callback_wait_us - previousDisplay.callback_wait_us) / (1000.0 * seconds));
    previousDisplay = display;
    const auto residency = aurora::gxm::residency_statistics();
    std::fprintf(stderr,
                 "MH3 native CDRAM: arena %.2f/%.2f MiB, textures %.2f MiB (%u live/%u pinned/%u GPU-written), retired textures %.2f MiB, models %.2f MiB + %.2f MiB retired, spill main/phycont "
                 "%.2f/%.2f MiB\n",
                 residency.arena_used / 1048576.0, residency.arena_capacity / 1048576.0, residency.texture_bytes / 1048576.0, residency.textures, residency.pinned_textures,
                 residency.gpu_written_textures, residency.retired_texture_bytes / 1048576.0, residency.model_bytes / 1048576.0, residency.retired_model_bytes / 1048576.0,
                 residency.main_spill_bytes / 1048576.0, residency.phycont_spill_bytes / 1048576.0);
#endif
    const auto ax = AxDspHle::GetMixWorkerStats();
    const uint64_t axJobs = ax.submitted - previousAx.submitted;
    std::fprintf(stderr, "MH3 AX: private %.1f jobs/s (%llu completed/%llu published), serial %llu rejected %llu failed %llu, snapshot %.2f KiB avg/%.2f max-ever, active PBs %u last\n",
                 axJobs / seconds, static_cast<unsigned long long>(ax.completed - previousAx.completed), static_cast<unsigned long long>(ax.published - previousAx.published),
                 static_cast<unsigned long long>(ax.serial - previousAx.serial), static_cast<unsigned long long>(ax.rejected - previousAx.rejected),
                 static_cast<unsigned long long>(ax.failed - previousAx.failed), axJobs ? double(ax.totalJobBytes - previousAx.totalJobBytes) / (1024.0 * axJobs) : 0.0, ax.maxJobBytes / 1024.0,
                 ax.lastActivePbs);
    std::fprintf(stderr,
                 "MH3 AX: prepare %.2f worker %.2f join %.2f publish %.2f ms/s; max-ever prepare %.3f worker %.3f join %.3f ms; storage %.3f MiB, worker %s thread %x core %d affinity %x "
                 "core0-checked %s; reverb owner\n",
                 double(ax.prepareNs - previousAx.prepareNs) / (1e6 * seconds), double(ax.workerNs - previousAx.workerNs) / (1e6 * seconds), double(ax.waitNs - previousAx.waitNs) / (1e6 * seconds),
                 double(ax.publishNs - previousAx.publishNs) / (1e6 * seconds), ax.maxPrepareNs / 1e6, ax.maxWorkerNs / 1e6, ax.maxWaitNs / 1e6, ax.storageBytes / 1048576.0,
                 ax.workerRunning ? "live" : "off", unsigned(ax.workerThread), ax.workerCore, unsigned(ax.workerAffinity), ax.core0Checked ? "yes" : "no");
    previousAx = ax;
    const auto audio = GetNativeAudioOutputStatistics();
    if (audio.available) {
        std::fprintf(stderr,
                     "MH3 audio: %u Hz, queued %.1f frames/s, output %.1f calls/s, ring %u/%u high-water frames; underflow %llu buffers, padded %.2f ms/s, dropped %llu blocks/%llu frames, errors "
                     "%llu, rate changes %llu\n",
                     audio.rateHz, double(audio.queuedFrames - previousAudio.queuedFrames) / seconds, double(audio.outputCalls - previousAudio.outputCalls) / seconds, audio.currentRingFrames,
                     audio.ringHighWaterFrames, static_cast<unsigned long long>(audio.underflowBuffers - previousAudio.underflowBuffers),
                     audio.rateHz ? double(audio.paddedFrames - previousAudio.paddedFrames) * 1000.0 / (audio.rateHz * seconds) : 0.0,
                     static_cast<unsigned long long>(audio.droppedBlocks - previousAudio.droppedBlocks), static_cast<unsigned long long>(audio.droppedFrames - previousAudio.droppedFrames),
                     static_cast<unsigned long long>(audio.outputErrors - previousAudio.outputErrors), static_cast<unsigned long long>(audio.rateChanges - previousAudio.rateChanges));
    }
    previousAudio = audio;
    const auto dma = GetNativeAudioDmaStatistics();
    const uint64_t dmaBlocks = dma.completedBlocks - previousDma.completedBlocks;
    std::fprintf(
        stderr, "MH3 audio: DMA %s %u Hz/%u bytes, %.1f services/s %.1f blocks/s %.1f frames/s (%.2f ms/s PCM), cap hits %llu, debt %.3f ms/%.3f max-ever, period %.3f ms, service gap %.3f max-ever\n",
        dma.enabled ? "on" : "off", dma.rateHz, dma.lengthBytes, double(dma.serviceCalls - previousDma.serviceCalls) / seconds, dmaBlocks / seconds,
        double(dma.completedFrames - previousDma.completedFrames) / seconds, double(dma.completedDurationNs - previousDma.completedDurationNs) / (1e6 * seconds),
        static_cast<unsigned long long>(dma.capHits - previousDma.capHits), dma.currentDebtNs / 1e6, dma.maxDebtNs / 1e6, dma.currentPeriodNs / 1e6, dma.maxServiceGapNs / 1e6);
    previousDma = dma;
    const auto feedback = ReadSceneFeedbackStats();
    std::fprintf(stderr, "MH3 feedback: %.1f allocations/s %.1f reuses/s, evictions %llu cancellations %llu; pool %.3f/%.3f MiB (%u/%u targets, %u retained/%u available), peak %.3f MiB/%u targets\n",
                 double(feedback.allocations - previousFeedback.allocations) / seconds, double(feedback.reuses - previousFeedback.reuses) / seconds,
                 static_cast<unsigned long long>(feedback.evictions - previousFeedback.evictions),
                 static_cast<unsigned long long>(feedback.capacity_cancellations - previousFeedback.capacity_cancellations), feedback.pool_bytes / 1048576.0, feedback.byte_limit / 1048576.0,
                 feedback.pool_targets, feedback.target_limit, feedback.retained_targets, feedback.available_targets, feedback.peak_pool_bytes / 1048576.0, feedback.peak_pool_targets);
    std::fprintf(stderr, "MH3 feedback: capture %.1f/s glare %.1f/s clone %.1f/s; last call %u targets (%u allocated/%u reused), %.3f MiB; untracked dimensions %llu\n",
                 double(feedback.capture_calls - previousFeedback.capture_calls) / seconds, double(feedback.glare_calls - previousFeedback.glare_calls) / seconds,
                 double(feedback.clone_calls - previousFeedback.clone_calls) / seconds, feedback.last_call_targets, feedback.last_call_allocations, feedback.last_call_reuses,
                 feedback.last_call_bytes / 1048576.0, static_cast<unsigned long long>(feedback.untracked_sizes));
    for (uint32_t i = 0; i < feedback.size_count; ++i) {
        const auto& size = feedback.sizes[i];
        if (i < previousFeedback.size_count && size.allocations == previousFeedback.sizes[i].allocations)
            continue;
        std::fprintf(stderr, "MH3 feedback: target %ux%u, %llu allocations/%llu reuses total, %.3f MiB/%u currently pooled\n", size.width, size.height,
                     static_cast<unsigned long long>(size.allocations), static_cast<unsigned long long>(size.reuses), size.pool_bytes / 1048576.0, size.pool_targets);
    }
    previousFeedback = feedback;
    const auto high = aurora::gfx::staging_high_water();
    std::fprintf(stderr, "MH3 staging: %llu splits, high water vert %llu idx %llu uniform %llu storage %llu (KiB)\n", static_cast<unsigned long long>(aurora::gfx::staging_split_count()),
                 static_cast<unsigned long long>(high[0] >> 10), static_cast<unsigned long long>(high[2] >> 10), static_cast<unsigned long long>(high[1] >> 10),
                 static_cast<unsigned long long>(high[3] >> 10));
    const auto nativeMemory = aurora_native_draw_stats();
    std::fprintf(stderr, "MH3 native draw memory: high water vert %llu idx %llu uniform %llu (KiB), capacity cancellations %llu\n",
                 static_cast<unsigned long long>(nativeMemory.vertex_high_water >> 10), static_cast<unsigned long long>(nativeMemory.index_high_water >> 10),
                 static_cast<unsigned long long>(nativeMemory.uniform_high_water >> 10), static_cast<unsigned long long>(nativeMemory.capacity_cancellations));
    const auto waits = aurora::gfx::staging_wait_counters();
    std::fprintf(stderr, "MH3 waits: GX staging %.2f ms/s (%llu waits), render total Finish %.2f ms/s (%llu calls)\n", double(waits[0] - lastWaits[0]) / (1000.0 * seconds),
                 static_cast<unsigned long long>(waits[2] - lastWaits[2]), double(waits[1] - lastWaits[1]) / (1000.0 * seconds), static_cast<unsigned long long>(waits[3] - lastWaits[3]));
    lastWaits = waits;
    last = now;
    lastTicks = ticks;
    lastCopies = guestCopies;
    lastPresents = presents;
    lastSkipped = skipped;
#endif
}

void ServiceHostGraphicsEvents() {
    if (!initialized)
        return;
    PollHostGraphicsEvents();
    if (quitRequested)
        throw HostQuit();
}

void CompleteHostGraphics() {
    if (!initialized)
        return;
    MH3_STAT(const auto completionStart = FrameProfile::Clock::now();)
    MH3_STAT(const auto afterQueue = FrameProfile::Clock::now();)
    FlushHostEfbPokes();
    if (guestStream.PendingBytes())
        throw std::runtime_error("GXDrawDone encountered a partial FIFO command");
    // Native submissions execute inline on the game thread and return while
    // the GPU is working. Completion notifications retain GPU input ownership.
    // Normal display copies seal at their FIFO boundary; this handles the
    // remaining manual/diagnostic path without submitting a partial frame.
    // An EFB peek submits what it needs itself (ReadHostEfbColor).
    if (frameActive && displayCopyPending)
        SealDisplayFrame();
#ifdef MH3_DEVELOPMENT
    const auto afterTail = FrameProfile::Clock::now();
    if (profile.interval) {
        auto ns = [](auto end, auto start) {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        };
        profile.completionNs += ns(afterTail, completionStart);
        profile.renderQueueNs += ns(afterQueue, completionStart);
        profile.tailNs += ns(afterTail, afterQueue);
        if (guestCopies > profile.measuredCopies) {
            if (profile.previousCopy != FrameProfile::Clock::time_point{}) {
                const auto copyNs = uint64_t(ns(afterTail, profile.previousCopy));
                profile.minCopyNs = std::min(profile.minCopyNs, copyNs);
                profile.maxCopyNs = std::max(profile.maxCopyNs, copyNs);
            }
            profile.previousCopy = afterTail;
            profile.measuredCopies = guestCopies;
        }
        const auto frames = guestCopies - profile.lastCopies;
        if (frames >= profile.interval) {
            const auto wallMs = ns(afterTail, profile.started) / 1e6 / frames;
            const auto cpuMs = double(std::clock() - profile.cpuStarted) * 1000 / CLOCKS_PER_SEC / frames;
#ifdef __vita__
            // Vita's libc clock is process elapsed time, including waits.
            constexpr const char* cpuClockLabel = "Vita process elapsed";
#else
            constexpr const char* cpuClockLabel = "process CPU";
#endif
            std::fprintf(stderr,
                         "MH3 profile: %llu display copies; %.2f ms wall/copy, %.2f ms %s/copy, "
                         "%.2f ms GX completion/copy (%.2f ms CPU decode/submit, %.2f ms graphics tail), "
                         "%.2f..%.2f ms copy interval, "
                         "%llu GX commands/copy, %llu draws/copy (%llu skipped before preparation), %llu arrays/copy, %llu textures/copy, "
                         "%llu EFB texture copies/copy (%.2f ms), %llu EFB snapshots/copy (%.2f ms), %llu EFB peeks/copy; "
                         "last frame %u host draws + %u merged into them; %.1f texture hashes/copy, %.1f TMEM invalidations/copy\n",
                         static_cast<unsigned long long>(frames), wallMs, cpuMs, cpuClockLabel, profile.completionNs / 1e6 / frames, profile.renderQueueNs / 1e6 / frames,
                         profile.tailNs / 1e6 / frames, profile.minCopyNs / 1e6, profile.maxCopyNs / 1e6,
                         static_cast<unsigned long long>((GuestGxStream::commandCount - profile.lastCommands) / frames),
                         static_cast<unsigned long long>((GuestGxStream::drawCount - profile.lastDraws) / frames),
                         static_cast<unsigned long long>((GuestGxStream::skippedDrawCount - profile.lastSkippedDraws) / frames),
                         static_cast<unsigned long long>((guestArrays - profile.lastArrays) / frames), static_cast<unsigned long long>((guestTextures - profile.lastTextures) / frames),
                         static_cast<unsigned long long>((guestTextureCopies - profile.lastTextureCopies) / frames), (guestTextureCopyNs - profile.lastTextureCopyNs) / 1e6 / frames,
                         static_cast<unsigned long long>((guestEfbSnapshots - profile.lastEfbSnapshots) / frames), (guestEfbSnapshotNs - profile.lastEfbSnapshotNs) / 1e6 / frames,
                         static_cast<unsigned long long>((guestEfbPeeks - profile.lastEfbPeeks) / frames), aurora_get_stats()->drawCallCount, aurora_get_stats()->mergedDrawCallCount,
                         double(GuestGxStream::textureHashCount - profile.lastTextureHashes) / frames, double(GuestGxStream::textureInvalidateCount - profile.lastTextureInvalidates) / frames);
            profile.lastTextureHashes = GuestGxStream::textureHashCount;
            profile.lastTextureInvalidates = GuestGxStream::textureInvalidateCount;
            profile.lastCopies = guestCopies;
            profile.lastCommands = GuestGxStream::commandCount;
            profile.lastArrays = guestArrays;
            profile.lastTextures = guestTextures;
            profile.lastDraws = GuestGxStream::drawCount;
            profile.lastSkippedDraws = GuestGxStream::skippedDrawCount;
            profile.lastTextureCopies = guestTextureCopies;
            profile.lastEfbSnapshots = guestEfbSnapshots;
            profile.lastEfbPeeks = guestEfbPeeks;
            profile.lastTextureCopyNs = guestTextureCopyNs;
            profile.lastEfbSnapshotNs = guestEfbSnapshotNs;
            profile.renderQueueNs = profile.tailNs = profile.completionNs = 0;
            profile.minCopyNs = UINT64_MAX;
            profile.maxCopyNs = 0;
            profile.started = afterTail;
            profile.cpuStarted = std::clock();
        }
    }
    static unsigned completions = 0;
    if (++completions <= 3 || completions % 300 == 0)
        std::fprintf(stderr, "MH3 graphics boundary %u: game-thread submission complete\n", completions);
#endif
    gpuSynchronized = true;
}

uint32_t ReadHostEfbColor(uint16_t x, uint16_t y) {
    MH3_STAT(++guestEfbPeeks;)
    if (!initialized || !gpuSynchronized || guestStream.PendingBytes())
        throw std::runtime_error("EFB color peek requires a completed GXDrawDone boundary");
    const auto& source = aurora::gpu::g_frameBuffer;
    if (!source.texture)
        throw std::runtime_error("EFB color texture is unavailable");
    if (efbPixels.empty() || efbWidth != source.width || efbHeight != source.height) {
        // Thousands of CPU peeks can follow one snapshot. New GX commands
        // discard efbPixels, so only the first peek needs the render-thread round trip.
        MH3_STAT(const auto started = FrameProfile::Clock::now();)
        if (frameActive) {
            // The peek reads this frame's draws: render it and submit them first.
            aurora::gfx::require_frame_render();
            if (!aurora_flush_draws())
                throw std::runtime_error("EFB peek could not resume the in-progress frame");
        }
        efbWidth = source.width;
        efbHeight = source.height;
        efbBgra = false; // The EFB is RGBA8, rows top-down.
        efbPitch = efbWidth * 4;
        efbPixels.resize(size_t(efbPitch) * efbHeight);
        aurora::gl::call([&] { aurora::gl::read_pixels(source.texture->gl(), 0, 0, efbWidth, efbHeight, efbPixels.data()); });
#ifdef MH3_DEVELOPMENT
        ++guestEfbSnapshots;
        guestEfbSnapshotNs += std::chrono::duration_cast<std::chrono::nanoseconds>(FrameProfile::Clock::now() - started).count();
        static unsigned snapshots = 0;
        if (++snapshots <= 3)
            std::fprintf(stderr, "MH3 EFB color snapshot %u: %ux%u, format %u\n", snapshots, efbWidth, efbHeight, guestStream.PixelFormat());
#endif
    }
    auto [logicalWidth, logicalHeight] = aurora::vi::configured_fb_size();
    if (!logicalWidth || !logicalHeight) {
        logicalWidth = efbWidth;
        logicalHeight = efbHeight;
    }
    if (x >= logicalWidth || y >= logicalHeight)
        throw std::runtime_error("EFB peek outside configured framebuffer");
    const uint32_t sx = uint64_t(2 * x + 1) * efbWidth / (2 * logicalWidth);
    const uint32_t sy = uint64_t(2 * y + 1) * efbHeight / (2 * logicalHeight);
    const auto* pixel = efbPixels.data() + sy * efbPitch + sx * 4;
    const uint32_t color = (uint32_t(pixel[3]) << 24) | (uint32_t(pixel[efbBgra ? 2 : 0]) << 16) | (uint32_t(pixel[1]) << 8) | pixel[efbBgra ? 0 : 2];
    uint32_t alphaRead = 0;
    if (!ReadGxRegister(0xCC001008, 2, alphaRead))
        throw std::runtime_error("PE alpha-read register unavailable");
    return ConvertEfbArgb(color, guestStream.PixelFormat(), alphaRead & 3);
}

void WriteHostEfbColor(uint16_t x, uint16_t y, uint32_t value) {
    (void)ReadHostEfbColor(x, y); // Ensure a coherent snapshot for masks and later peeks.
    uint32_t blend = 0, alpha = 0, depth = 0, destinationAlpha = 0;
    ReadGxRegister(0xCC001002, 2, blend);
    ReadGxRegister(0xCC001006, 2, alpha);
    ReadGxRegister(0xCC001000, 2, depth);
    ReadGxRegister(0xCC001004, 2, destinationAlpha);
    static bool traced = false;
    if (!traced) {
        std::fprintf(stderr, "MH3 EFB poke state: blend=%04x alpha=%04x depth=%04x destinationAlpha=%04x\n", blend, alpha, depth, destinationAlpha);
        traced = true;
    }
    if ((blend & 0x803) || ((depth & 1) && ((depth >> 1) & 7) != 7))
        throw std::runtime_error("Unsupported EFB poke blending/logic/depth comparison");
    const unsigned a = value >> 24, threshold = alpha & 255;
    const bool compares[]{false, a<threshold, a == threshold, a <= threshold, a> threshold, a != threshold, a >= threshold, true};
    if (!compares[(alpha >> 8) & 7])
        return;
    if (destinationAlpha & 256)
        value = (value & 0xFFFFFF) | ((destinationAlpha & 255) << 24);
    value = ConvertEfbArgb(value, guestStream.PixelFormat(), 2);
    auto [width, height] = aurora::vi::configured_fb_size();
    if (!width || !height) {
        width = efbWidth;
        height = efbHeight;
    }
    const uint32_t x0 = uint64_t(x) * efbWidth / width, x1 = uint64_t(x + 1) * efbWidth / width;
    const uint32_t y0 = uint64_t(y) * efbHeight / height, y1 = uint64_t(y + 1) * efbHeight / height;
    if (!(blend & 0x18))
        return;
    for (uint32_t row = y0; row < y1; ++row)
        for (uint32_t col = x0; col < x1; ++col) {
            auto* pixel = efbPixels.data() + row * efbPitch + col * 4;
            if (blend & 8) {
                pixel[efbBgra ? 2 : 0] = (value >> 16) & 255;
                pixel[1] = (value >> 8) & 255;
                pixel[efbBgra ? 0 : 2] = value & 255;
            }
            if (blend & 16)
                pixel[3] = value >> 24;
        }
    if (!efbDirty) {
        efbMinX = x0;
        efbMinY = y0;
        efbMaxX = x1;
        efbMaxY = y1;
    } else {
        efbMinX = std::min(efbMinX, x0);
        efbMinY = std::min(efbMinY, y0);
        efbMaxX = std::max(efbMaxX, x1);
        efbMaxY = std::max(efbMaxY, y1);
    }
    efbDirty = true;
}

int RunEfbCheck(int argc, char** argv) {
    InitializeGraphics(argc, argv);
    struct Shutdown {
        ~Shutdown() { ShutdownGraphics(); }
    } shutdown;
    const auto& target = aurora::gpu::g_frameBuffer;
    const bool bgra = false;
    for (unsigned pass = 0; pass < 2; ++pass) {
        const uint32_t expected = pass ? 0xFFABCDEF : 0xFF123456;
        uint8_t pixels[16];
        for (unsigned i = 0; i < 4; ++i) {
            pixels[i * 4] = bgra ? expected & 255 : (expected >> 16) & 255;
            pixels[i * 4 + 1] = (expected >> 8) & 255;
            pixels[i * 4 + 2] = bgra ? (expected >> 16) & 255 : expected & 255;
            pixels[i * 4 + 3] = 255;
        }
        aurora::gl::write_texture(target.texture, 0, 0, 0, 2, 2, pixels, 8);
        efbPixels.clear();
        WriteGxRegister(0xCC001008, 2, 1);
        for (auto address : {0x08000000u, 0x88000004u, 0xC8001004u}) {
            uint32_t value;
            if (!ReadEfbColor32(address, value) || value != expected)
                throw std::runtime_error("GLES EFB color/row/alias check failed");
        }
        WriteGxRegister(0xCC001008, 2, 0);
        uint32_t value;
        if (!ReadEfbColor32(0xC8000000, value) || value != (expected & 0xFFFFFF))
            throw std::runtime_error("GLES EFB alpha-read check failed");
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
        throw std::runtime_error("GLES EFB dirty-rectangle upload failed");
    frameActive = aurora_begin_frame();
    if (!frameActive)
        throw std::runtime_error("EFB preservation check could not begin a frame");
    gpuSynchronized = false;
    const auto beforeFence = aurora::gfx::current_frame();
    aurora::gfx::split_staging_batch();
    if (aurora::gfx::current_frame() != beforeFence)
        throw std::runtime_error("Staging capacity split incorrectly advanced the display frame");
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
    std::puts("MH3 GLES EFB color readback/poke check passed");
    return 0;
}

int RunGraphicsCheck(int argc, char** argv) {
    if (!aurora_configure_native_target(640, 448) || aurora_configure_native_target(0, 448))
        throw std::runtime_error("Native target dimensions were not validated");
    InitializeGraphics(argc, argv);
    struct Shutdown {
        ~Shutdown() { ShutdownGraphics(); }
    } shutdown;
    const auto base = aurora::vi::configured_fb_size();
    const auto visible = aurora::vi::visible_fb_size();
    if (base.x != 640 || base.y != 448 || visible.x != 640 || visible.y != 448)
        throw std::runtime_error("Native target retained legacy scratch rows or accepted zero dimensions");
    if (!aurora::gl::on_render_thread())
        throw std::runtime_error("Native context is not owned by the initializing thread");
    StartNativeRenderThread();
    if (aurora::gl::on_render_thread())
        throw std::runtime_error("Game thread kept the graphics context");
    bool taskOnRenderThread = false;
    QueueNativeGraphicsTask(NativeTaskOrder::BeforeDraws, [&] { taskOnRenderThread = aurora::gl::on_render_thread(); });
    unsigned recorded = 0;
    for (unsigned frame = 0; frame < 30; ++frame) {
        ServiceHostGraphicsEvents();
        if (BeginNativeGraphicsFrame())
            ++recorded;
        EndNativeGraphicsFrame();
    }
    StopNativeRenderThread();
    if (!aurora::gl::on_render_thread())
        throw std::runtime_error("Render thread did not return the graphics context");
    if (!taskOnRenderThread)
        throw std::runtime_error("Graphics task did not run on the render thread");
    // This diagnostic alone consumes a GPU result: the frame clear is black.
    const auto& target = aurora::gpu::g_frameBuffer;
    uint8_t pixel[4]{};
    aurora::gl::read_pixels(target.texture->gl(), 0, 0, 1, 1, pixel);
    if (recorded && (pixel[0] > 1 || pixel[1] > 1 || pixel[2] > 1 || pixel[3] != 255))
        throw std::runtime_error("Native renderer clear/present pixel validation failed");
    std::fprintf(stderr, "MH3 renderer check: 30 logical native frames, %u recorded, render thread owner, tasks on the render thread, pixel %u,%u,%u,%u; no GX calls\n", recorded,
                 pixel[0], pixel[1], pixel[2], pixel[3]);
    return 0;
}
