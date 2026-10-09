#include "gx_host.h"
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
#include <deque>
#include <exception>
#include <mutex>
#include <pthread.h>
#include <vector>
#include <cstring>
#include <aurora/aurora.h>
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
#include <thread>
#include <memory>
#include <string>
#if defined(MH3_ENABLE_GX_DIAGNOSTICS) || defined(MH3_DEVELOPMENT)
#include "gx/gx.hpp"
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
// Aurora's event polling (game thread) and frame boundaries (GX worker) take turns.
std::mutex auroraFrameMutex;

// The GX worker: the guest FIFO bytes go to it in chunks, and it frames them
// (GuestGxStream), decodes them into Aurora and begins and seals the host
// frames, while the game thread runs on. GXDrawDone drains it, the hardware's
// point where the CPU may reuse what the GPU was reading; EFB peeks and pokes
// come after that. The guest RAM the commands point at (arrays, display lists,
// textures) is read when the worker gets to them, as the GPU reads it when it
// gets to them.
//
// Between Submits the filling chunk's free tail is the FIFO window
// (gGxFifoWindow, memory_access.h) the translated FIFO writes store into; the
// chunk's size is taken back from it before anything else touches the chunk.
class GxWorker {
public:
    explicit GxWorker(void (*process)(const uint8_t*, size_t)) : process(process), filling(kChunk) {
        thread = std::thread([this] { Run(); });
        pthread_setname_np(thread.native_handle(), "MH3 GX");
    }
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
    // Waits until the worker processed everything submitted; rethrows its failure.
    void Drain() {
        CloseWindow();
        if (filling.size) HandOff();
        std::unique_lock lock(mutex);
        idle.wait(lock, [&] { return queue.empty() && !busy; });
        RethrowFailure();
    }
    void CloseFifoWindow() { CloseWindow(); }
    void Stop() {
        CloseWindow();
        {
            std::lock_guard lock(mutex);
            stop = true;
            queue.clear();
        }
        work.notify_one();
        thread.join();
    }

private:
    static constexpr size_t kChunk = 16 * 1024, kMaxQueued = 8 * 1024 * 1024;
    struct Chunk {
        explicit Chunk(size_t capacity) : bytes(new uint8_t[capacity]), capacity(capacity) {}
        std::unique_ptr<uint8_t[]> bytes;
        size_t size = 0, capacity;
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
        idle.wait(lock, [&] { return queued < kMaxQueued || failure; }); // a worker far behind stalls the game
        RethrowFailure();
        queued += filling.size;
        queue.push_back(std::move(filling));
        if (!spare.empty()) { filling = std::move(spare.back()); spare.pop_back(); }
        else filling = Chunk(kChunk);
        lock.unlock();
        work.notify_one();
    }
    void Run() {
        std::unique_lock lock(mutex);
        for (;;) {
            work.wait(lock, [&] { return stop || !queue.empty(); });
            if (stop) return;
            auto chunk = std::move(queue.front());
            queue.pop_front();
            busy = true;
            const bool skip = failure != nullptr; // after a failure, until the game thread sees it
            lock.unlock();
            std::exception_ptr error;
            if (!skip) {
                try { process(chunk.bytes.get(), chunk.size); }
                catch (...) { error = std::current_exception(); }
            }
            lock.lock();
            if (error) failure = error;
            queued -= chunk.size;
            busy = false;
            chunk.size = 0;
            if (chunk.capacity == kChunk) spare.push_back(std::move(chunk)); // not an outsized one
            idle.notify_all();
        }
    }
    void (*const process)(const uint8_t*, size_t);
    Chunk filling; // game thread only
    std::mutex mutex;
    std::condition_variable work, idle;
    std::deque<Chunk> queue;
    std::vector<Chunk> spare;
    size_t queued = 0;
    bool busy = false, stop = false;
    std::exception_ptr failure;
    std::thread thread;
};
// Null until the first GX bytes, or when the GX runs on the game thread
// (MH3_GX_WORKER=0, per-draw diagnostics).
GxWorker* gxWorker = nullptr;
void DrainGxWorker() { if (gxWorker) gxWorker->Drain(); }

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
std::atomic<uint64_t> guestCopies{0}; // counted on the GX worker
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
    uint64_t workerNs = 0, gpuNs = 0, completionNs = 0;
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
        if (gxWorker) { gxWorker->Stop(); gxWorker = nullptr; }
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
    // MH3 renders only through OpenGL ES 3.0 (aurora-gles).
    config.desiredBackend = BACKEND_OPENGLES;
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
    // Aurora falls back to another backend when GLES fails; never run on one.
    if (info.backend != BACKEND_OPENGLES) throw std::runtime_error("Aurora did not select OpenGL ES");
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
// Frames guest FIFO bytes into Aurora: on the GX worker, or on the game thread
// without one.
void ProcessGuestGxBytes(const uint8_t* data, size_t size) {
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
    }, [](const GuestGxStream::TextureCopy& copy) {
        MH3_STAT(const auto started = FrameProfile::Clock::now();)
        GXSetTexCopySrc(copy.x, copy.y, copy.sourceWidth, copy.sourceHeight);
        GXSetTexCopyDst(copy.width, copy.height, static_cast<GXTexFmt>(copy.format), copy.halfScale ? GX_TRUE : GX_FALSE);
        auto* destination = reinterpret_cast<void*>(copy.destination);
#ifdef MH3_ENABLE_EFB_COPY_TRACE
        EfbCopyTrace::BeforeCopy(copy.destination, copy.bytes);
#endif
        GXCopyTex(destination, copy.clear ? GX_TRUE : GX_FALSE);
        // No RAM readback: the copy stays on the GPU and Aurora binds it when
        // the guest samples this address. MH3 was not seen reading copy bytes
        // from the CPU; MH3_ENABLE_EFB_COPY_TRACE reports any access that does.
#ifdef MH3_ENABLE_EFB_COPY_TRACE
        EfbCopyTrace::AfterCopy(copy.destination, copy.guestDestination, copy.bytes, copy.width, copy.height, copy.format);
#endif
        MH3_STAT(++guestTextureCopies;
                 guestTextureCopyNs += std::chrono::duration_cast<std::chrono::nanoseconds>(FrameProfile::Clock::now() - started).count();)
    });
    // GXCopyDisp is the presentation boundary. Seal its owned draw data now,
    // letting GPU encoding overlap guest work before GXDrawDone/next GX use.
    if (displayCopyPending) SealDisplayFrame();
}
}

#ifdef MH3_DEVELOPMENT
static std::vector<uint8_t>* gxCapture = nullptr;
void CaptureGuestGx(std::vector<uint8_t>* into) {
    if (gxWorker) gxWorker->CloseFifoWindow();
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
        // The first GX since GXDrawDone (the worker is idle): the CPU's EFB
        // pixels go in ahead of it, and its draws end the EFB snapshot.
        char name[] = "MH3";
        char* argv[]{name, nullptr};
        InitializeGraphics(1, argv);
        FlushHostEfbPokes();
        efbPixels.clear();
        gpuSynchronized = false;
        static const bool useWorker = [] {
#if defined(MH3_ENABLE_GX_DIAGNOSTICS) || defined(MH3_ENABLE_EFB_COPY_TRACE)
            return false;
#else
#ifdef MH3_DEVELOPMENT
            // MH3_GX_WORKER=0: GX on the game thread; the draw log needs it there too.
            const char* v = std::getenv("MH3_GX_WORKER");
            if ((v && v[0] == '0') || std::getenv("MH3_GX_DRAW_LOG")) return false;
#endif
            return true;
#endif
        }();
        if (useWorker && !gxWorker) gxWorker = new GxWorker(ProcessGuestGxBytes);
    }
    if (gxWorker) gxWorker->Submit(data, size);
    else ProcessGuestGxBytes(data, size);
}

template <size_t N>
void SubmitGuestGxFixed(const uint8_t* data) {
    GX_HLE_FIFO_WriteBytesInline(data, N);
}
template void SubmitGuestGxFixed<1>(const uint8_t*);
template void SubmitGuestGxFixed<2>(const uint8_t*);
template void SubmitGuestGxFixed<4>(const uint8_t*);

// MH3_STATS: once per second, separate game speed from rendered FPS.
// Game speed is retrace periods the frame pacer stepped (frames * budget)
// versus the nominal VI rate, since the game logic is fixed-step per frame;
// game FPS counts display copies; host
// FPS counts frames the render thread actually swapped.
void ReportHostSpeed([[maybe_unused]] double ticks, [[maybe_unused]] double nominalHz) {
#ifdef MH3_DEVELOPMENT
    static const bool enabled = std::getenv("MH3_STATS") != nullptr;
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
    std::fprintf(stderr, "MH3 staging: %llu splits, high water vert %zu idx %zu uniform %zu storage %zu (KiB)\n",
                 static_cast<unsigned long long>(aurora::gfx::staging_split_count()),
                 size_t(high[0] >> 10), size_t(high[2] >> 10), size_t(high[1] >> 10), size_t(high[3] >> 10));
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
    DrainGxWorker();
    MH3_STAT(const auto afterWorker = FrameProfile::Clock::now();)
    FlushHostEfbPokes();
    if (guestStream.PendingBytes()) throw std::runtime_error("GXDrawDone encountered a partial FIFO command");
    // GXDrawDone never waits for the render thread or the GPU: nothing reads
    // rendered results back on the CPU (no copy readback, no peeks since bloom is
    // gone), and frameskip absorbs a slow GPU instead. Mid-frame it does not
    // submit either; MH3's display routine calls it after the next frame's first
    // GX writes, and an early submission there would force the frameskip
    // decision while the previous frame is certainly still rendering. An EFB
    // peek submits what it needs itself (ReadHostEfbColor).
    if (frameActive && displayCopyPending) SealDisplayFrame();
#ifdef MH3_DEVELOPMENT
    const auto afterGpu = FrameProfile::Clock::now();
    if (profile.interval) {
        auto ns = [](auto end, auto start) { return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(); };
        profile.completionNs += ns(afterGpu, completionStart);
        profile.workerNs += ns(afterWorker, completionStart);
        profile.gpuNs += ns(afterGpu, afterWorker);
        if (guestCopies > profile.measuredCopies) {
            if (profile.previousCopy != FrameProfile::Clock::time_point{}) {
                const auto copyNs = uint64_t(ns(afterGpu, profile.previousCopy));
                profile.minCopyNs = std::min(profile.minCopyNs, copyNs);
                profile.maxCopyNs = std::max(profile.maxCopyNs, copyNs);
            }
            profile.previousCopy = afterGpu;
            profile.measuredCopies = guestCopies;
        }
        const auto frames = guestCopies - profile.lastCopies;
        if (frames >= profile.interval) {
            const auto wallMs = ns(afterGpu, profile.started) / 1e6 / frames;
            const auto cpuMs = double(std::clock() - profile.cpuStarted) * 1000 / CLOCKS_PER_SEC / frames;
            std::fprintf(stderr,
                "MH3 profile: %llu display copies; %.2f ms wall/copy, %.2f ms process CPU/copy, "
                "%.2f ms GX completion/copy (%.2f ms worker, %.2f ms GPU queue), "
                "%.2f..%.2f ms copy interval, "
                "%llu GX commands/copy, %llu draws/copy, %llu arrays/copy, %llu textures/copy, "
                "%llu EFB texture copies/copy (%.2f ms), %llu EFB snapshots/copy (%.2f ms), %llu EFB peeks/copy; "
                "last frame %u host draws + %u merged into them; %.1f texture hashes/copy, %.1f TMEM invalidations/copy\n",
                static_cast<unsigned long long>(frames), wallMs, cpuMs, profile.completionNs / 1e6 / frames,
                profile.workerNs / 1e6 / frames, profile.gpuNs / 1e6 / frames,
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
            profile.workerNs = profile.gpuNs = profile.completionNs = 0;
            profile.minCopyNs = UINT64_MAX; profile.maxCopyNs = 0;
            profile.started = afterGpu;
            profile.cpuStarted = std::clock();
        }
    }
    static unsigned completions = 0;
    if (++completions <= 3 || completions % 300 == 0)
        std::fprintf(stderr, "MH3 GX draw completion %u: GPU work finished\n", completions);
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
    if (frames != 30) throw std::runtime_error("GLES frame check timed out");
    std::fprintf(stderr, "MH3 GLES renderer check: %u frames submitted\n", frames);
    return 0;
}
