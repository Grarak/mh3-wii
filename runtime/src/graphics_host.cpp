#include "gx_host.h"
#ifdef __vita__
#include "../vita/platform.h"
#include "gxm/razor.hpp"
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
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <vector>
#include <cstring>
#include <aurora/aurora.h>
#include <aurora/generated_texture.hpp>
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
namespace aurora::gfx { uint32_t current_frame() noexcept; void preserve_next_efb_frame() noexcept; void split_staging_batch(); void require_frame_render() noexcept;
    std::array<uint64_t, 4> staging_high_water() noexcept; uint64_t staging_split_count() noexcept; }

namespace aurora::gx::fifo {
void init();
uint32_t process(const uint8_t*, uint32_t, bool);
}
namespace {
bool initialized = false;
bool frameActive = false;
bool gpuSynchronized = true, displayCopyPending = false, hasBegunFrame = false;
uint32_t captureInterval = 0;
uint32_t captureStart = 0;
std::filesystem::path capturePath;
GuestGxStream guestStream;
std::vector<uint8_t> efbPixels;
uint32_t efbPitch = 0, efbWidth = 0, efbHeight = 0;
bool efbBgra = false;
bool efbDirty = false;
uint32_t efbMinX = 0, efbMinY = 0, efbMaxX = 0, efbMaxY = 0;
// Aurora's event polling (game thread) and render-thread frame boundaries take turns.
std::mutex auroraFrameMutex;

// The game fills owned FIFO chunks and posts them to Aurora's render thread.
// That thread frames/decodes GX, prepares batches and submits native draws.
// GXDrawDone waits for CPU command consumption so guest arrays can be reused;
// it does not explicitly finish the GPU. No separate decoding thread exists.
// Between submissions, the filling chunk's tail is the translated FIFO window.
class GxQueue {
public:
    explicit GxQueue(void (*process)(const uint8_t*, size_t)) : process(process), filling(kChunk) {}
    void Submit(const uint8_t* data, size_t size) {
        CloseWindow();
        if (filling.size + size > filling.capacity) [[unlikely]] {
            if (filling.size) HandOff();
            if (size > filling.capacity) filling = Chunk(size);
        }
        std::memcpy(filling.bytes.get() + filling.size, data, size);
        filling.size += size;
        gGxFifoWindow = {filling.bytes.get() + filling.size, filling.bytes.get() + filling.capacity};
    }
    void SubmitTask(std::function<void()> task) {
        CloseWindow();
        if (filling.size) HandOff();
        Chunk chunk(0);
        chunk.task = std::move(task);
        {
            std::lock_guard lock(mutex);
            RethrowFailure();
            ++pending;
        }
        Post(std::move(chunk));
    }
    void Drain() {
        CloseWindow();
        if (filling.size) HandOff();
        std::unique_lock lock(mutex);
        idle.wait(lock, [&] { return pending == 0; });
        RethrowFailure();
    }
    void CloseFifoWindow() { CloseWindow(); }
    void Stop() {
        CloseWindow();
        std::unique_lock lock(mutex);
        stopping = true;
        // Already posted callbacks capture this object; keep it alive until
        // the render queue consumes or cancels every owned chunk.
        idle.wait(lock, [&] { return pending == 0; });
    }

private:
    static constexpr size_t kChunk = 16 * 1024, kMaxQueued = 8 * 1024 * 1024;
    struct Chunk {
        explicit Chunk(size_t capacity) : bytes(capacity ? new uint8_t[capacity] : nullptr), capacity(capacity) {}
        std::unique_ptr<uint8_t[]> bytes;
        size_t size = 0, capacity;
        std::function<void()> task;
    };
    void CloseWindow() {
        if (!gGxFifoWindow.end) return;
        filling.size = size_t(gGxFifoWindow.next - filling.bytes.get());
        gGxFifoWindow = {};
    }
    void RethrowFailure() {
        if (failure) std::rethrow_exception(std::exchange(failure, nullptr));
    }
    void HandOff() {
        std::unique_lock lock(mutex);
        RethrowFailure();
        idle.wait(lock, [&] { return queued < kMaxQueued || failure; });
        RethrowFailure();
        queued += filling.size;
        ++pending;
        auto chunk = std::move(filling);
        if (!spare.empty()) { filling = std::move(spare.back()); spare.pop_back(); }
        else filling = Chunk(kChunk);
        lock.unlock();
        Post(std::move(chunk));
    }
    void Post(Chunk chunk) {
        aurora::gl::post([this, chunk = std::move(chunk)]() mutable { Consume(std::move(chunk)); });
    }
    void Consume(Chunk chunk) {
        bool skip;
        {
            std::lock_guard lock(mutex);
            skip = stopping || failure != nullptr;
        }
        std::exception_ptr error;
#ifdef __vita__
        uint64_t cpuStarted = 0;
#endif
        if (!skip) {
            try {
                if (!aurora::gl::on_render_thread()) throw std::runtime_error("GX queue consumed outside the render thread");
#ifdef __vita__
                if (!reportedThread) {
                    std::fprintf(stderr, "MH3 GX queue: decoded by GXM render thread 0x%08x on core 1\n", unsigned(sceKernelGetThreadId()));
                    reportedThread = true;
                }
#endif
#ifdef __vita__
                cpuStarted = aurora::gxm::razor_begin_cpu_work();
#endif
                if (chunk.task) chunk.task(); else process(chunk.bytes.get(), chunk.size);
            } catch (...) { error = std::current_exception(); }
        }
#ifdef __vita__
        aurora::gxm::razor_end_cpu_work(cpuStarted);
#endif
        {
            std::lock_guard lock(mutex);
            if (error && !failure) failure = error;
            queued -= chunk.size;
            --pending;
            chunk.size = 0;
            if (chunk.capacity == kChunk) spare.push_back(std::move(chunk));
        }
        idle.notify_all();
    }
    void (*const process)(const uint8_t*, size_t);
    Chunk filling; // game thread only
    std::mutex mutex;
    std::condition_variable idle;
    std::vector<Chunk> spare;
    size_t queued = 0, pending = 0;
    bool stopping = false, reportedThread = false;
    std::exception_ptr failure;
};
// Null until the first GX bytes, or when the GX runs on the game thread
// (MH3_GX_WORKER=0, per-draw diagnostics).
GxQueue* gxQueue = nullptr;
void DrainGxQueue() { if (gxQueue) gxQueue->Drain(); }

void FlushHostEfbPokes() {
    if (!efbDirty) return;
    if (!gpuSynchronized) throw std::runtime_error("EFB pokes require a completed framebuffer");
    // Queued on the render thread behind every earlier draw, ahead of later ones.
    const uint32_t width = efbMaxX - efbMinX, height = efbMaxY - efbMinY;
    std::vector<uint8_t> rect(size_t(width) * height * 4);
    for (uint32_t row = 0; row < height; ++row)
        std::memcpy(rect.data() + size_t(row) * width * 4,
                    efbPixels.data() + size_t(efbMinY + row) * efbPitch + efbMinX * 4, size_t(width) * 4);
    aurora::gl::write_texture(aurora::gpu::g_frameBuffer.texture, 0, efbMinX, efbMinY, width, height,
                              std::move(rect), width * 4);
    // GXDrawDone is a synchronization point, not a Wii EFB clear. The next
    // host frame must retain the CPU's pixels for subsequent GXCopyTex/draws.
    if (!frameActive) aurora::gfx::preserve_next_efb_frame();
    efbDirty = false;
}
std::atomic<uint64_t> guestCopies{0}; // counted on the render thread
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
        MH3_STAT(if (GuestGxStream::commandCount) std::fprintf(stderr, "MH3 GX bridge: %llu commands consumed, %llu array conversions, %llu texture bindings, %llu display copies, %llu draws, %llu texture copies\n",
            static_cast<unsigned long long>(GuestGxStream::commandCount), static_cast<unsigned long long>(guestArrays), static_cast<unsigned long long>(guestTextures), static_cast<unsigned long long>(guestCopies),
            static_cast<unsigned long long>(GuestGxStream::drawCount), static_cast<unsigned long long>(guestTextureCopies)));
        if (gxQueue) { gxQueue->Stop(); delete gxQueue; gxQueue = nullptr; }
        aurora::clear_generated_textures();
        aurora_shutdown(); initialized = false; frameActive = false;
    }
}
void InitializeGraphics(int argc, char** argv) {
    if (initialized) return;
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
        if (n > 1) { config.windowWidth *= n; config.windowHeight *= n; }
    }
#endif
    config.msaa = 1;
    config.maxTextureAnisotropy = 1;
    const auto info = aurora_initialize(argc, argv, &config);
    if (info.initializationStatus != AURORA_INITIALIZATION_SUCCESS)
        throw std::runtime_error(info.initializationError ? info.initializationError : "Aurora initialization failed");
#ifdef __vita__
    const char* discRoot = std::getenv("MH3_DISC_DIR");
    const auto nativePack = std::filesystem::path(discRoot ? discRoot : "ux0:/data/mh3-wii") / "native-textures.bin";
    if (std::filesystem::exists(nativePack)) aurora::gl::open_native_texture_pack(nativePack.string().c_str());
#endif
    initialized = true;
#ifdef MH3_DEVELOPMENT
    // MH3_RENDER_SCALE=n: render the EFB at n times the native resolution,
    // whatever the window size (captures are at that size).
    if (const char* v = std::getenv("MH3_RENDER_SCALE")) {
        const float n = std::strtof(v, nullptr);
        if (n > 0) VISetFrameBufferScale(n);
    }
    // The draw log looks at one command per packet.
    if (std::getenv("MH3_GX_DRAW_LOG")) guestStream.coalesce = false;
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
        if (!value || value > UINT32_MAX) throw std::runtime_error("MH3_PROFILE_FRAMES must be a positive 32-bit number");
        profile.interval = static_cast<uint32_t>(value);
        profile.started = FrameProfile::Clock::now();
        profile.cpuStarted = std::clock();
    }
#endif
    std::atexit(ShutdownGraphics);
    if (info.backend != config.desiredBackend) throw std::runtime_error("Aurora did not select the required renderer");
    aurora::gx::fifo::init();
    // Frameskip: a frame is dropped when the render thread is still busy with the
    // previous one. MH3_FRAMESKIP=0 renders every frame (debugging).
    aurora_set_frameskip(true);
#ifdef MH3_DEVELOPMENT
    const char* frameskip = std::getenv("MH3_FRAMESKIP");
    if (frameskip && frameskip[0] == '0') aurora_set_frameskip(false);
    if (const char* path = std::getenv("MH3_CAPTURE_PATH")) {
        if (const char* interval = std::getenv("MH3_CAPTURE_INTERVAL")) {
            const auto value = std::stoul(interval);
            if (!value || value >= UINT32_MAX)
                throw std::runtime_error("MH3_CAPTURE_INTERVAL must be between 1 and 4294967294");
            captureInterval = static_cast<uint32_t>(value);
            capturePath = path;
            // MH3_CAPTURE_START=n: no captures before frame n.
            if (const char* start = std::getenv("MH3_CAPTURE_START")) captureStart = uint32_t(std::stoul(start));
        } else {
            const char* frame = std::getenv("MH3_CAPTURE_FRAME");
            aurora_request_frame_capture(frame ? std::stoul(frame) : 30, path);
        }
    }
#endif
}
}

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
            if (end == v) break;
            v = *end == ',' ? end + 1 : end;
        }
        return words;
    }();
    if (stepLog) {
        std::fprintf(stepLog, "%llu %u %.4f", (unsigned long long)frame, GameStep::Ticks(), GameStep::Scale());
        for (uint32_t word : logWords) std::fprintf(stepLog, " %08x", Memory::Read32(word));
        std::fprintf(stepLog, "\n");
    }
#endif
    if (captureInterval && frame >= captureStart && frame % captureInterval == 0) {
        const auto path = capturePath.parent_path() /
            (capturePath.stem().string() + "-" + std::to_string(frame) + ".bmp");
        aurora_request_frame_capture(frame, path.c_str());
    }
    {
        std::lock_guard lock(auroraFrameMutex);
        aurora_end_frame();
    }
    frameActive = false; displayCopyPending = false;
}

bool quitRequested = false;

// Reads the host events; a quit is kept for ServiceHostGraphicsEvents.
void PollHostGraphicsEvents() {
    std::lock_guard lock(auroraFrameMutex);
    for (const auto* event = aurora_update(); event && event->type != AURORA_NONE; ++event)
        if (event->type == AURORA_EXIT) quitRequested = true;
}

namespace {
// Decode guest FIFO bytes on the render thread (game-thread diagnostics remain opt-in).
void ProcessGuestGxBytes(const uint8_t* data, size_t size) {
#ifdef MH3_DEVELOPMENT
    if (guestStream.traceDraw) {
        std::array<uint32_t, 4> callers{};
        if (const auto* cpu = TryGetCpuContext()) {
            callers[0] = cpu->lr;
            uint32_t stack = cpu->gpr[1];
            for (unsigned i = 1; i < callers.size(); ++i) {
                const uint32_t physical = stack & 0x3fffffff;
                if ((stack & 3) || !((physical < Memory::kMem1Size - 8) ||
                    (physical >= Memory::kMem2PhysicalBase && physical < Memory::kMem2PhysicalEnd - 8))) break;
                const uint32_t next = Memory::Read32(stack);
                callers[i] = Memory::Read32(stack + 4);
                if (next <= stack) break;
                stack = next;
            }
        }
        aurora::gx::fifo::set_capture_callers(callers.data());
    }
#endif
    if (!frameActive) {
        std::lock_guard lock(auroraFrameMutex);
        if (hasBegunFrame) aurora::gfx::preserve_next_efb_frame();
        frameActive = aurora_begin_frame();
        hasBegunFrame = true;
        if (!frameActive) throw std::runtime_error("Aurora could not begin the guest GX frame");
    }
    guestStream.Write(std::span<const uint8_t>(data, size), [](auto packet) {
#ifdef MH3_DEVELOPMENT
        // MH3_GX_DRAW_LOG=file,from,to: per draw of frames from..to, the
        // frame, the draw's index, its primitive and vertex count, a hash of
        // its bytes and of the loaded position matrices, and the current
        // position matrix's translation.
        static FILE* drawLog = nullptr;
        static uint64_t logFrom = 0, logTo = 0;
        static const bool drawLogInit = [] {
            const char* v = std::getenv("MH3_GX_DRAW_LOG");
            if (!v) return true;
            std::string spec = v;
            const auto a = spec.find(','), b = spec.rfind(',');
            if (a == std::string::npos || a == b) return true;
            logFrom = std::stoull(spec.substr(a + 1, b - a - 1));
            logTo = std::stoull(spec.substr(b + 1));
            drawLog = std::fopen(spec.substr(0, a).c_str(), "w");
            return true;
        }();
        (void)drawLogInit;
        static uint64_t drawFrame = ~0ull;
        static uint32_t drawIndex = 0;
        if (drawLog && packet[0] >= 0x80 && packet[0] <= 0xBF) {
            const uint64_t frame = aurora::gfx::current_frame();
            if (frame != drawFrame) { drawFrame = frame; drawIndex = 0; }
            if (frame >= logFrom && frame <= logTo) {
                auto fnv = [](const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
                    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
                    return h;
                };
                const auto& st = aurora::gx::g_gxState;
                uint64_t mh = 1469598103934665603ull;
                for (const auto& m : st.pnMtx) mh = fnv(&m.pos, sizeof(m.pos), mh);
                const auto& cur = st.pnMtx[st.currentPnMtx / 3].pos;
                std::fprintf(drawLog, "%llu %u %02x %u %016llx %016llx %u %.3f %.3f %.3f\n", (unsigned long long)frame,
                             drawIndex, packet[0], unsigned(packet[1] << 8 | packet[2]),
                             (unsigned long long)fnv(packet.data(), packet.size()), (unsigned long long)mh,
                             st.currentPnMtx, cur.m0.w(), cur.m1.w(), cur.m2.w());
            }
            ++drawIndex;
        }
#endif
#ifdef MH3_ENABLE_GX_DIAGNOSTICS
        GxDiagnosticDraw diagnostic(packet[0] >= 0x80 && packet[0] <= 0xBF);
        if (diagnostic.skip) return;
#endif
        // A packet holds one or more complete commands. Aurora yields before a
        // draw when the current staging batch has no room for it. Split only
        // after process releases its renderer lock; this keeps the EFB,
        // viewport and pending presentation within the same frame.
        size_t consumed = aurora::gx::fifo::process(packet.data(), packet.size(), true);
        while (consumed != packet.size()) {
            aurora::gfx::split_staging_batch();
            const size_t more = aurora::gx::fifo::process(packet.data() + consumed, packet.size() - consumed, true);
            if (more == 0) throw std::runtime_error("Guest GX command does not fit after staging capacity submission");
            consumed += more;
        }
#ifdef MH3_DEVELOPMENT
        if (packet[0] == 0x50 && packet[1] == 0 && packet[2] >= 0x10 && packet[2] <= 0x1F) ++guestArrays;
        if (packet[0] == 0x50 && packet[1] == 0 && packet[2] == 0x30) ++guestTextures;
#endif
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
#ifdef MH3_ENABLE_EFB_COPY_TRACE
        EfbCopyTrace::EndFrame();
#endif
#ifdef MH3_ENABLE_GX_DIAGNOSTICS
        UpdateGxDiagnosticMode();
#endif
        displayCopyPending = true;
    }, [](const GuestGxStream::EfbClear& copy) {
        // The stream retains only a texture-copy command's clear side effect.
        GXSetTexCopySrc(copy.x, copy.y, copy.sourceWidth, copy.sourceHeight);
        GXCopyTex(nullptr, GX_TRUE);
    });
    // GXCopyDisp is the presentation boundary. Seal its owned draw data now,
    // letting GPU encoding overlap guest work before GXDrawDone/next GX use.
    if (displayCopyPending) SealDisplayFrame();
}
}

#ifdef MH3_DEVELOPMENT
static std::vector<uint8_t>* gxCapture = nullptr;
void CaptureGuestGx(std::vector<uint8_t>* into) {
    if (gxQueue) gxQueue->CloseFifoWindow();
    gxCapture = into;
}
#endif

void SubmitGuestGxBytes(const uint8_t* data, size_t size) {
#ifdef MH3_DEVELOPMENT
    if (gxCapture) [[unlikely]] {
        gxCapture->insert(gxCapture->end(), data, data + size);
        return;
    }
#endif
    if (gpuSynchronized) {
        // The first GX since GXDrawDone (the FIFO queue is drained): the CPU's EFB
        // pixels go in ahead of it, and its draws end the EFB snapshot.
        char name[] = "MH3";
        char* argv[]{name, nullptr};
        InitializeGraphics(1, argv);
        FlushHostEfbPokes();
        efbPixels.clear();
        gpuSynchronized = false;
        static const bool useRenderQueue = [] {
#if defined(MH3_ENABLE_GX_DIAGNOSTICS) || defined(MH3_ENABLE_EFB_COPY_TRACE)
            return false;
#else
#ifdef MH3_DEVELOPMENT
            // MH3_GX_WORKER=0: GX on the game thread; the draw log needs it there too.
            const char* v = std::getenv("MH3_GX_WORKER");
            if ((v && v[0] == '0') || std::getenv("MH3_GX_DRAW_LOG") ||
                std::getenv("MH3_GX_SOURCE_CAPTURE")) return false;
#endif
            return true;
#endif
        }();
        if (useRenderQueue && !gxQueue) gxQueue = new GxQueue(ProcessGuestGxBytes);
    }
    if (gxQueue) gxQueue->Submit(data, size);
    else ProcessGuestGxBytes(data, size);
}

void SubmitHostGraphicsTask(std::function<void()> task) {
    char name[] = "MH3";
    char* argv[]{name, nullptr};
    InitializeGraphics(1, argv);
    if (gxQueue) gxQueue->SubmitTask(std::move(task));
    else task();
}

template <size_t N>
void SubmitGuestGxFixed(const uint8_t* data) {
    GX_HLE_FIFO_WriteBytesInline(data, N);
}
template void SubmitGuestGxFixed<1>(const uint8_t*);
template void SubmitGuestGxFixed<2>(const uint8_t*);
template void SubmitGuestGxFixed<4>(const uint8_t*);

// Development stats default on; MH3_STATS=0 disables the once-per-second line.
// Game speed is retrace periods the frame pacer stepped (frames * budget)
// versus the nominal VI rate, since the game logic is fixed-step per frame;
// game FPS counts display copies; host
// FPS counts frames the render thread actually swapped.
void ReportHostSpeed([[maybe_unused]] double ticks, [[maybe_unused]] double nominalHz) {
#ifdef __vita__
    ReportVitaMemory();
#endif
#ifdef MH3_DEVELOPMENT
    static const bool enabled = [] {
        const char* value = std::getenv("MH3_STATS");
        return !value || std::strcmp(value, "0") != 0;
    }();
    if (!enabled || !initialized) return;
    using Clock = std::chrono::steady_clock;
    static auto last = Clock::now();
    static double lastTicks = ticks;
    static uint64_t lastCopies = guestCopies, lastPresents = aurora_presented_frame_count(),
                    lastSkipped = aurora_skipped_frame_count();
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - last).count();
    if (seconds < 1.0) return;
    const uint64_t presents = aurora_presented_frame_count(), skipped = aurora_skipped_frame_count();
    const double tickHz = (ticks - lastTicks) / seconds;
    std::fprintf(stderr,
        "MH3 stats: game speed %.1f%% (%.1f/%.1f Hz paced), game %.1f fps (display copies), host %.1f fps (presents), "
        "%.1f fps skipped, %lld frames in flight, NI %s\n",
        100.0 * tickHz / nominalHz, tickHz, nominalHz, (guestCopies - lastCopies) / seconds,
        (presents - lastPresents) / seconds, (skipped - lastSkipped) / seconds,
        static_cast<long long>(guestCopies - presents - skipped), g_mkwHostNiActive ? "set" : "clear");
    const auto high = aurora::gfx::staging_high_water();
    std::fprintf(stderr, "MH3 staging: %llu splits, high water vert %llu idx %llu uniform %llu storage %llu (KiB)\n",
                 static_cast<unsigned long long>(aurora::gfx::staging_split_count()),
                 static_cast<unsigned long long>(high[0] >> 10), static_cast<unsigned long long>(high[2] >> 10),
                 static_cast<unsigned long long>(high[1] >> 10), static_cast<unsigned long long>(high[3] >> 10));
    static std::array<uint64_t, 4> lastWaits{};
    const auto waits = aurora::gfx::staging_wait_counters();
    std::fprintf(stderr, "MH3 waits: GX staging %.2f ms/s (%llu waits), render total Finish %.2f ms/s (%llu calls)\n",
                 double(waits[0] - lastWaits[0]) / (1000.0 * seconds),
                 static_cast<unsigned long long>(waits[2] - lastWaits[2]),
                 double(waits[1] - lastWaits[1]) / (1000.0 * seconds),
                 static_cast<unsigned long long>(waits[3] - lastWaits[3]));
    lastWaits = waits;
    last = now;
    lastTicks = ticks;
    lastCopies = guestCopies;
    lastPresents = presents;
    lastSkipped = skipped;
#endif
}

void ServiceHostGraphicsEvents() {
    if (!initialized) return;
    PollHostGraphicsEvents();
    if (quitRequested) throw HostQuit();
}

void CompleteHostGraphics() {
    if (!initialized) return;
    MH3_STAT(const auto completionStart = FrameProfile::Clock::now();)
    DrainGxQueue();
    MH3_STAT(const auto afterQueue = FrameProfile::Clock::now();)
    FlushHostEfbPokes();
    if (guestStream.PendingBytes()) throw std::runtime_error("GXDrawDone encountered a partial FIFO command");
    // GXDrawDone consumes queued CPU commands before the guest reuses their
    // source arrays. It does not explicitly wait for GPU completion; nothing
    // reads rendered results back on the CPU, and frameskip handles a busy GPU. Mid-frame it does not
    // submit either; MH3's display routine calls it after the next frame's first
    // GX writes, and an early submission there would force the frameskip
    // decision while the previous frame is certainly still rendering. An EFB
    // peek submits what it needs itself (ReadHostEfbColor).
    if (frameActive && displayCopyPending) SealDisplayFrame();
#ifdef MH3_DEVELOPMENT
    const auto afterTail = FrameProfile::Clock::now();
    if (profile.interval) {
        auto ns = [](auto end, auto start) { return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(); };
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
                "%llu GX commands/copy, %llu draws/copy, %llu arrays/copy, %llu textures/copy, "
                "%llu EFB texture copies/copy (%.2f ms), %llu EFB snapshots/copy (%.2f ms), %llu EFB peeks/copy; "
                "last frame %u host draws + %u merged into them; %.1f texture hashes/copy, %.1f TMEM invalidations/copy\n",
                static_cast<unsigned long long>(frames), wallMs, cpuMs, cpuClockLabel, profile.completionNs / 1e6 / frames,
                profile.renderQueueNs / 1e6 / frames, profile.tailNs / 1e6 / frames,
                profile.minCopyNs / 1e6, profile.maxCopyNs / 1e6,
                static_cast<unsigned long long>((GuestGxStream::commandCount - profile.lastCommands) / frames),
                static_cast<unsigned long long>((GuestGxStream::drawCount - profile.lastDraws) / frames),
                static_cast<unsigned long long>((guestArrays - profile.lastArrays) / frames),
                static_cast<unsigned long long>((guestTextures - profile.lastTextures) / frames),
                static_cast<unsigned long long>((guestTextureCopies - profile.lastTextureCopies) / frames),
                (guestTextureCopyNs - profile.lastTextureCopyNs) / 1e6 / frames,
                static_cast<unsigned long long>((guestEfbSnapshots - profile.lastEfbSnapshots) / frames),
                (guestEfbSnapshotNs - profile.lastEfbSnapshotNs) / 1e6 / frames,
                static_cast<unsigned long long>((guestEfbPeeks - profile.lastEfbPeeks) / frames),
                aurora_get_stats()->drawCallCount, aurora_get_stats()->mergedDrawCallCount,
                double(GuestGxStream::textureHashCount - profile.lastTextureHashes) / frames,
                double(GuestGxStream::textureInvalidateCount - profile.lastTextureInvalidates) / frames);
            profile.lastTextureHashes = GuestGxStream::textureHashCount;
            profile.lastTextureInvalidates = GuestGxStream::textureInvalidateCount;
            profile.lastCopies = guestCopies;
            profile.lastCommands = GuestGxStream::commandCount;
            profile.lastArrays = guestArrays;
            profile.lastTextures = guestTextures;
            profile.lastDraws = GuestGxStream::drawCount;
            profile.lastTextureCopies = guestTextureCopies;
            profile.lastEfbSnapshots = guestEfbSnapshots;
            profile.lastEfbPeeks = guestEfbPeeks;
            profile.lastTextureCopyNs = guestTextureCopyNs;
            profile.lastEfbSnapshotNs = guestEfbSnapshotNs;
            profile.renderQueueNs = profile.tailNs = profile.completionNs = 0;
            profile.minCopyNs = UINT64_MAX; profile.maxCopyNs = 0;
            profile.started = afterTail;
            profile.cpuStarted = std::clock();
        }
    }
    static unsigned completions = 0;
    if (++completions <= 3 || completions % 300 == 0)
        std::fprintf(stderr, "MH3 GX draw completion %u: CPU GX queue drained\n", completions);
#endif
    gpuSynchronized = true;
}

uint32_t ReadHostEfbColor(uint16_t x, uint16_t y) {
    MH3_STAT(++guestEfbPeeks;)
    if (!initialized || !gpuSynchronized || guestStream.PendingBytes())
        throw std::runtime_error("EFB color peek requires a completed GXDrawDone boundary");
    const auto& source = aurora::gpu::g_frameBuffer;
    if (!source.texture) throw std::runtime_error("EFB color texture is unavailable");
    if (efbPixels.empty() || efbWidth != source.width || efbHeight != source.height) {
        // Thousands of CPU peeks can follow one snapshot. New GX commands
        // discard efbPixels, so only the first peek needs the render-thread round trip.
        MH3_STAT(const auto started = FrameProfile::Clock::now();)
        if (frameActive) {
            // The peek reads this frame's draws: render it and submit them first.
            aurora::gfx::require_frame_render();
            if (!aurora_flush_draws()) throw std::runtime_error("EFB peek could not resume the in-progress frame");
        }
        efbWidth = source.width; efbHeight = source.height;
        efbBgra = false; // The EFB is RGBA8, rows top-down.
        efbPitch = efbWidth * 4;
        efbPixels.resize(size_t(efbPitch) * efbHeight);
        aurora::gl::call([&] {
            aurora::gl::read_pixels(source.texture->gl(), 0, 0, efbWidth, efbHeight, efbPixels.data());
        });
#ifdef MH3_DEVELOPMENT
        ++guestEfbSnapshots;
        guestEfbSnapshotNs += std::chrono::duration_cast<std::chrono::nanoseconds>(FrameProfile::Clock::now() - started).count();
        static unsigned snapshots = 0;
        if (++snapshots <= 3) std::fprintf(stderr, "MH3 EFB color snapshot %u: %ux%u, format %u\n",
            snapshots, efbWidth, efbHeight, guestStream.PixelFormat());
#endif
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
    const auto& target = aurora::gpu::g_frameBuffer;
    const bool bgra = false;
    for (unsigned pass = 0; pass < 2; ++pass) {
        const uint32_t expected = pass ? 0xFFABCDEF : 0xFF123456;
        uint8_t pixels[16];
        for (unsigned i = 0; i < 4; ++i) {
            pixels[i*4] = bgra ? expected & 255 : (expected >> 16) & 255;
            pixels[i*4+1] = (expected >> 8) & 255;
            pixels[i*4+2] = bgra ? (expected >> 16) & 255 : expected & 255;
            pixels[i*4+3] = 255;
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
    if (!frameActive) throw std::runtime_error("EFB preservation check could not begin a frame");
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
    if (frames != 30) throw std::runtime_error("Renderer frame check timed out");
    std::fprintf(stderr, "MH3 renderer check: %u frames submitted\n", frames);
    return 0;
}
