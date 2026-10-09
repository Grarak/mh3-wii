// Game -> render thread frame packets (native_frame_packet.h).
#include "native_frame_packet.h"
#include "fatal.h"
#include "bytes_equal.h"
#include "native_graphics.h"
#include "native_shade_snapshot.h"
#include "frame_split.h"
#include "game_step.h"
#include "isa/ppc_isa_thread_state.h"
#include "runtime/native/adapters/model_scope_bridge.h"
#include "model_render_prep.h"
#include "spin_signal.h"
#include "frame_limit.h"
#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#include "port_check.h"
#endif
#include <aurora/aurora.h>
#include "gl/gl.hpp"
#include "gl/device.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <chrono>
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#elif defined(__linux__) && defined(MH3_DEVELOPMENT)
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#endif

namespace aurora::gfx {
uint32_t current_frame() noexcept;
}

namespace {
// Per-frame geometry high water is about 200 KiB of vertices (Alatreon quest).
constexpr size_t VertexArenaBytes = 2u << 20, IndexArenaBytes = 512u << 10;

// A packet's compact quads: 3640 quads, 512 KiB.
constexpr uint32_t QuadArenaVertices = 4 * 3640;

enum class Kind : uint8_t { Model, MaterialGeometry, Geometry, DepthClear, Capture, Glare, Clone, Quads };
using StateIndices = std::array<uint32_t, NATIVE_STATE_COMPONENTS>;
struct Record {
    Kind kind;
    bool textured;
    uint32_t payload;
    StateIndices state;
};

// Compact effect quads (RecordNativeQuads): the vertices in the packet's quad
// arena, which the GPU reads in place, and the draw's normal and colour.
struct Quads {
    AuroraNativeQuadVertex* vertices;
    uint32_t count;
    float normal[3];
    uint8_t color[4];
};

struct Feedback {
    const void* identity;
    const void* source;
    aurora::SceneCaptureParameters capture;
    aurora::SceneGlareParameters glare;
};

// Snapshots of one state component, stored only when the component changed.
template <class T>
struct Pool {
    std::vector<T> items;
    uint32_t count = 0, current = 0, revision = 0;

    // diverged: a deferred model item changed the render thread's copy of this
    // component after the current snapshot; equal contents no longer mean an
    // equal render state, so the next changed capture always gets an index.
    bool valid = false, diverged = false;
    void Reset() { count = 0, valid = false, diverged = false; }
};

struct Packet {
    uint32_t slot = 0;
    uint64_t sequence = 0;
    bool render = false, canceled = false;
    std::string capture;
    std::vector<Record> records;
    std::vector<AuroraNativeMaterialGeometry> materialGeometry;
    std::vector<AuroraNativeGeometry> geometry;
    std::vector<Feedback> feedback;
    uint32_t feedbackCount = 0;
    Pool<NativeDrawSnapshot> draws;
    Pool<NativeMaterialSnapshot> materials;
    Pool<NativeLightingSnapshot> lighting;
    Pool<NativePostSnapshot> posts;
    Pool<NativeTextureSnapshot> textures;
    Pool<NativeColorSnapshot> colors;
    std::unique_ptr<uint8_t[]> vertices{new uint8_t[VertexArenaBytes]}, indices{new uint8_t[IndexArenaBytes]};
    size_t vertexCursor = 0, indexCursor = 0;

    // GPU-readable, mapped at the first use (aurora_map_native_quad_memory):
    // the render thread draws the quads from here, without a copy, so the
    // slot is recorded again only once the GPU finished it (gpuDone).
    std::vector<Quads> quads;
    AuroraNativeQuadVertex* quadArena = nullptr;
    uint32_t quadCursor = 0;
    aurora::gl::GpuFence gpuDone{};

    // The records of the game-side tail (BeginNativeFramePacketTail), moved
    // to the end at publication; tailBegin SIZE_MAX when none.
    size_t tailBegin = SIZE_MAX, tailEnd = 0;
#ifdef MH3_DEVELOPMENT
    uint64_t publishedNs = 0; // RenderMonotonicNs at publication
    uint64_t inputNs = 0;     // RenderMonotonicNs at the begin, just before the logic reads the input
    uint64_t gameUs = 0;      // game time of the frame (GameStep::Elapsed), for pacing statistics
#endif
    void Reset(uint64_t next, bool admitted) {
        sequence = next;
        render = admitted;
        canceled = false;
        capture.clear();
        records.clear();
        materialGeometry.clear();
        geometry.clear();
        quads.clear();
        quadCursor = 0;
        feedbackCount = 0;
        draws.Reset();
        materials.Reset();
        lighting.Reset();
        posts.Reset();
        textures.Reset();
        colors.Reset();
        vertexCursor = indexCursor = 0;
        tailBegin = SIZE_MAX;
        tailEnd = 0;
    }
};

struct Task {
    uint64_t sequence;
    std::function<void()> run;
};

// A ring by sequence: packet s in packets[s % kNativeFramePacketSlots]. The
// render thread takes packets in order, so it renders one while the game
// thread may have up to two published behind it and record a fourth.
std::array<Packet, kNativeFramePacketSlots> packets;
std::thread renderThread;
std::thread::id gameThread;
std::atomic<bool> running{false}, stopping{false}, failed{false};
#if defined(__cpp_exceptions)
std::exception_ptr failure; // the render thread's, for the game thread to report
#endif

// published: packets handed over (render thread waits on it); completed: the
// sequence of the last packet the render thread finished (game waits on it).
SpinSignal published;
std::atomic<uint64_t> completed{0};

// taken: the sequence of the newest packet the render thread took (pacing);
// lastPublished: the newest packet published (game thread writes it).
std::atomic<uint64_t> taken{0}, lastPublished{0};

// Advanced after each take and each completion (and a failure): the game
// thread limiter waits on it.
SpinSignal takenSignal;
#ifdef MH3_DEVELOPMENT
struct RenderCounters {
    std::atomic<uint32_t> revision{0};
    std::atomic<uint64_t> packets{0}, drawPackets{0}, wallNs{0}, drawWallNs{0};

    // Time blocked with no packet to replay, and how many acquisitions waited.
    std::atomic<uint64_t> waitNs{0}, waits{0};

    // Of those waits, the time from the packet's publication to its replay
    // start (the wake latency), and how many took over a millisecond.
    std::atomic<uint64_t> wakeNs{0}, wakes{0}, lateWakes{0};
    std::atomic<int32_t> thread{-1};
#ifdef __linux__
    std::atomic<clockid_t> cpuClock{-1};
#endif
} renderCounters;

uint64_t RenderMonotonicNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

mh3::statistics::OwnerCpuSample RenderCpuSample(int32_t thread) noexcept {
    mh3::statistics::OwnerCpuSample sample;
    sample.thread = thread;

    if (thread < 0)
        return sample;
#ifdef __vita__
    SceKernelThreadInfo info{};
    info.size = sizeof(info);

    if (sceKernelGetThreadInfo(thread, &info) >= 0) {
        sample.running_us = info.runClocks;
        sample.core = info.currentCpuId;
        sample.affinity = info.currentCpuAffinityMask;
        sample.available = true;
    }
#elif defined(__linux__)
    timespec time{};
    const auto clock = renderCounters.cpuClock.load(std::memory_order_acquire);

    if (clock != clockid_t(-1) && clock_gettime(clock, &time) == 0) {
        sample.running_us = uint64_t(time.tv_sec) * 1000000 + uint64_t(time.tv_nsec) / 1000;
        sample.available = true;
    }

    cpu_set_t affinity;

    if (sched_getaffinity(thread, sizeof(affinity), &affinity) == 0) {
        uint32_t mask = 0;

        for (unsigned core = 0; core < 32; ++core)
            if (CPU_ISSET(core, &affinity))
                mask |= uint32_t(1) << core;
        sample.affinity = int32_t(mask);
    }

    // sched_getcpu() here would report the game thread. /proc reports the
    // sampled render task's last CPU, including while that task is sleeping.
    char path[80], text[1024];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", thread);

    if (FILE* file = std::fopen(path, "r")) {
        if (std::fgets(text, sizeof(text), file)) {
            if (char* cursor = std::strrchr(text, ')')) {
                ++cursor;

                for (unsigned field = 3; field <= 39; ++field) {
                    while (*cursor == ' ')
                        ++cursor;

                    if (!*cursor)
                        break;

                    char* end = cursor;

                    while (*end && *end != ' ')
                        ++end;

                    if (field == 39)
                        sample.core = int32_t(std::strtol(cursor, nullptr, 10));

                    cursor = end;
                }
            }
        }

        std::fclose(file);
    }
#endif
    return sample;
}
#endif
#ifdef MH3_DEVELOPMENT
// Where the render thread is, for stall reports: its stage, the packet and
// the record (index << 8 | kind) or task index within the stage.
enum class RenderStage : uint32_t { Waiting, BeforeTasks, Begin, Draw, End, AfterTasks, StopTasks };
std::atomic<uint32_t> renderStage{0}, renderStep{0};
std::atomic<uint64_t> renderPacket{0};
void Stage(RenderStage stage, uint32_t step = 0) noexcept {
    renderStage.store(uint32_t(stage), std::memory_order_relaxed);
    renderStep.store(step, std::memory_order_relaxed);
}
#define MH3_RENDER_STAGE(...) Stage(__VA_ARGS__)

// MH3_PROFILE=render: the render thread names the part of a packet's replay
// it is in (NativeRenderSplit) with one relaxed store; a sampler thread
// counts what it finds every 250 us. No clock reads on the render thread.
bool renderSplit = false;
std::atomic<unsigned> renderPart{NativeRenderSplit::Waiting};
std::array<std::atomic<uint64_t>, NativeRenderSplit::Parts> renderPartSamples{};
std::atomic<uint64_t> renderSplitPackets{0};

// Detached for the process's life; on core 0 on the Vita, beside the audio
// mixer and the display callback, off the game and render threads' cores.
void SampleRenderParts() {
#ifdef __vita__
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
#endif
    for (;;) {
        renderPartSamples[renderPart.load(std::memory_order_relaxed)].fetch_add(1, std::memory_order_relaxed);
#ifdef __vita__
        sceKernelDelayThread(250);
#else
        std::this_thread::sleep_for(std::chrono::microseconds(250));
#endif
    }
}
#define MH3_RENDER_PART(part)                                                                                                                                  \
    do {                                                                                                                                                       \
        if (renderSplit)                                                                                                                                       \
            renderPart.store(NativeRenderSplit::part, std::memory_order_relaxed);                                                                              \
    } while (0)
#else
#define MH3_RENDER_STAGE(...)
#define MH3_RENDER_PART(part)
#endif
bool synchronous = false;

// The game thread limiter (the launcher's setting, FrameLimitSetting): at
// most one published packet waits for the render thread; see
// PaceNativeFramePacket. Latched at the render thread's start.
bool frameLimit = true;
bool paced = true;
std::mutex taskLock;
std::deque<Task> beforeTasks, afterTasks;

// Game thread.
Packet* recording = nullptr;
uint64_t sequence = 0;
#ifdef MH3_DEVELOPMENT
uint64_t pacingWaitNs = 0;
uint32_t pacingWaits = 0;
#endif
struct FeedbackTarget {
    uint32_t width, height;
    uint64_t revision;
};

std::unordered_map<const void*, FeedbackTarget> feedbackTargets;
uint64_t feedbackRevision = 0;

// The game thread, or the draw thread while it runs the frame's draw for the
// waiting game thread (docs/pipelined-draw.md).
bool OnProducer() {
    return std::this_thread::get_id() == gameThread || PpcThread::CurrentThread() == PpcThread::Draw;
}

void RequireGame() {
    if (running && !OnProducer())
        MH3_THROW(std::runtime_error("Frame packet recorded outside the game thread"));
}

// The draw thread's scene graph walk beside the game thread's recording.
std::atomic<bool> prepareOnDraw{false};

// Records come from one producer at a time: not from the draw thread while it
// walks the scene graph beside the game thread.
void RequireRecorder() {
#ifdef MH3_DEVELOPMENT
    if (prepareOnDraw.load(std::memory_order_relaxed) && PpcThread::CurrentThread() == PpcThread::Draw)
        MH3_THROW(std::runtime_error("Frame packet recorded during the draw thread's scene prepare"));
#endif
}

// Without exceptions (release builds) a render-thread failure stops the game
// where it happens, and nothing comes here.
void RethrowFailure() {
#if defined(__cpp_exceptions)
    if (failed.load(std::memory_order_acquire))
        std::rethrow_exception(failure);
#endif
}

// Whether packet `next` may be recorded: the packet before it in its slot is
// one the render thread finished, or skipped (it took a newer one and never
// goes back).
bool SlotReleased(uint64_t next) {
    if (next <= kNativeFramePacketSlots || !running.load(std::memory_order_relaxed))
        return true;

    const uint64_t previous = next - kNativeFramePacketSlots;
    return completed.load(std::memory_order_acquire) >= previous || taken.load(std::memory_order_acquire) > previous;
}

// Released, and its quads, drawn in place, are no longer read by the GPU (the
// fence is set by the render thread before it advanced `completed`).
bool SlotFree(uint64_t next) {
    return SlotReleased(next) && (!running.load(std::memory_order_relaxed) || aurora::gl::gpu_fence_done(packets[next % kNativeFramePacketSlots].gpuDone));
}

#ifdef MH3_DEVELOPMENT
// Per component: draw records, captures (revision advanced) and stored copies.
uint32_t snapshotCounts[3][NATIVE_STATE_COMPONENTS];

// Material geometry submissions, and those appended to the previous draw.
uint32_t geometrySubmitted, geometryMerged;

// Why a submission started a draw of its own: the record before it was not
// material geometry, its draw/material/lighting/textures/colours snapshot
// differed (one count per differing component), or its geometry did not
// follow on in the arenas.
uint32_t geometryUnmerged[7];
#endif
// Current (optional): whether the live state is a snapshot's, without
// copying it. A changed component whose state is one of the last few stored
// snapshots (a particle flipbook alternating its textures) references that
// one instead of storing a copy.
template <class T, void (*Capture)(T&), bool (*Current)(const T&) = nullptr>
uint32_t Component(Pool<T>& pool, NativeStateComponent component) {
#ifdef MH3_DEVELOPMENT
    ++snapshotCounts[0][component];
#endif
    const uint32_t revision = nativeStateRevision[NativeOwnerIndex(NativeOwner::Game)][component];

    if (pool.valid && pool.revision == revision)
        return pool.current;
#ifdef MH3_DEVELOPMENT
    ++snapshotCounts[1][component];
#endif
    if constexpr (Current != nullptr) {
        // Not after a deferred model item changed the render thread's copy:
        // equal contents no longer mean an equal render state.
        if (pool.valid && !pool.diverged) {
            const uint32_t candidates[] = {pool.current, pool.count - 1, pool.count - 2, pool.count - 3};

            for (uint32_t i = 0; i < 4; ++i) {
                const uint32_t index = candidates[i];

                if (index >= pool.count || (i && index == pool.current))
                    continue;

                if (Current(pool.items[index])) {
                    pool.revision = revision;
                    pool.current = index;
                    return index;
                }
            }
        }
    }

    if (pool.count == pool.items.size())
        pool.items.emplace_back();

    auto& next = pool.items[pool.count];
    Capture(next);
    pool.revision = revision;

    // Producers republish unchanged state; keep one copy of equal contents.
    if (pool.valid && !pool.diverged && BytesEqual(&next, &pool.items[pool.current], sizeof next))
        return pool.current;
#ifdef MH3_DEVELOPMENT
    ++snapshotCounts[2][component];
#endif
    pool.valid = true;
    pool.diverged = false;
    pool.current = pool.count++;
    return pool.current;
}

StateIndices CaptureState(bool usesPost = true) {
    auto& p = *recording;
    StateIndices state;
    state[NATIVE_STATE_DRAW] = Component<NativeDrawSnapshot, CaptureNativeDrawSnapshot>(p.draws, NATIVE_STATE_DRAW);
    state[NATIVE_STATE_MATERIAL] = Component<NativeMaterialSnapshot, CaptureNativeMaterialSnapshot>(p.materials, NATIVE_STATE_MATERIAL);
    state[NATIVE_STATE_LIGHTING] = Component<NativeLightingSnapshot, CaptureNativeLightingSnapshot>(p.lighting, NATIVE_STATE_LIGHTING);

    // Pre-transformed material geometry reads no post matrix: it takes the
    // current copy without capturing a changed one (the next draw that reads
    // them captures it).
    if (usesPost || !p.posts.valid || p.posts.diverged)
        state[NATIVE_STATE_POST] = Component<NativePostSnapshot, CaptureNativePostSnapshot>(p.posts, NATIVE_STATE_POST);
    else
        state[NATIVE_STATE_POST] = p.posts.current;

    state[NATIVE_STATE_TEXTURES] = Component<NativeTextureSnapshot, CaptureNativeTextureSnapshot, NativeTextureSnapshotCurrent>(p.textures, NATIVE_STATE_TEXTURES);
    state[NATIVE_STATE_COLORS] = Component<NativeColorSnapshot, CaptureNativeColorSnapshot>(p.colors, NATIVE_STATE_COLORS);
    return state;
}

void Push(Kind kind, uint32_t payload, bool textured = false) {
    RequireRecorder();
    recording->records.push_back({kind, textured, payload, CaptureState()});
}
#ifdef MH3_DEVELOPMENT
// MH3_PORT_CHECK=EffectBatch (development): checks that the state a material
// geometry draw is replayed with (its draw, material + colour, lighting and
// texture snapshots, shared with the draws batched with it) is the complete
// Game state at its submission, which an unbatched, fully copied draw would
// have captured.
void CheckBatchedState(const Packet& p, const StateIndices& state) {
    static const bool enabled = PortCheck::Enabled("EffectBatch");

    if (!enabled)
        return;

    static uint64_t checks = 0, mismatches = 0;
    ++checks;
    std::string differs;
    NativeMaterialSnapshot material, replayed = p.materials.items[state[NATIVE_STATE_MATERIAL]];
    CaptureNativeMaterialSnapshot(material);
    ApplyNativeColorSnapshot(replayed.material, p.colors.items[state[NATIVE_STATE_COLORS]]);

    if (!BytesEqual(&material.material, &replayed.material, sizeof material.material) || material.depthBeforeTexture != replayed.depthBeforeTexture)
        differs += " material";

    NativeDrawSnapshot draw;
    CaptureNativeDrawSnapshot(draw);
    const auto& drawn = p.draws.items[state[NATIVE_STATE_DRAW]];

    if (!BytesEqual(&draw.draw, &drawn.draw, sizeof draw.draw) || draw.scissor != drawn.scissor || draw.viewportKnown != drawn.viewportKnown || draw.scissorKnown != drawn.scissorKnown ||
        draw.rasterKnown != drawn.rasterKnown)
        differs += " draw";
    NativeLightingSnapshot lighting;
    CaptureNativeLightingSnapshot(lighting);
    const auto& lit = p.lighting.items[state[NATIVE_STATE_LIGHTING]];

    if (!BytesEqual(&lighting.lights, &lit.lights, sizeof lighting.lights) || !BytesEqual(&lighting.preparedLights, &lit.preparedLights, sizeof lighting.preparedLights) ||
        !BytesEqual(&lighting.fog, &lit.fog, sizeof lighting.fog) || !BytesEqual(&lighting.preparedFog, &lit.preparedFog, sizeof lighting.preparedFog) || lighting.fogKnown != lit.fogKnown)
        differs += " lighting";
    // Field by field: the bindings have padding the producers leave unset.
    NativeTextureSnapshot textures;
    CaptureNativeTextureSnapshot(textures);
    const auto& recorded = p.textures.items[state[NATIVE_STATE_TEXTURES]];
    bool sameTextures = textures.valid == recorded.valid && !std::memcmp(textures.palettes.data(), recorded.palettes.data(), sizeof textures.palettes);

    for (unsigned unit = 0; unit < 8 && sameTextures; ++unit) {
        if (!(textures.valid >> unit & 1))
            continue;

        const auto &x = textures.textures[unit], &y = recorded.textures[unit];
        const auto &a = x.source, &b = y.source;
        sameTextures = a.resource_address == b.resource_address && a.payload_address == b.payload_address && a.asset_id == b.asset_id && a.format == b.format && a.width == b.width &&
                       a.height == b.height && a.levels == b.levels && a.min_lod == b.min_lod && a.max_lod == b.max_lod && a.generation == b.generation && a.indexed == b.indexed &&
                       a.mutable_source == b.mutable_source && x.sampler.wrap_s == y.sampler.wrap_s && x.sampler.wrap_t == y.sampler.wrap_t && x.sampler.min_filter == y.sampler.min_filter &&
                       x.sampler.mag_filter == y.sampler.mag_filter && x.sampler.anisotropy == y.sampler.anisotropy && x.sampler.lod_bias == y.sampler.lod_bias &&
                       x.sampler.bias_clamp == y.sampler.bias_clamp && x.sampler.edge_lod == y.sampler.edge_lod && x.map_id == y.map_id && x.tlut_id == y.tlut_id;
    }

    if (!sameTextures)
        differs += " textures";

    if (!differs.empty() && ++mismatches <= 20)
        std::printf("MH3 port check: EffectBatch replays draw %llu with other state:%s\n", static_cast<unsigned long long>(checks), differs.c_str());
    else if (differs.empty() && (checks & (checks - 1)) == 0)
        std::printf("MH3 port check: EffectBatch matches the original (%llu draws, %llu with other state)\n", static_cast<unsigned long long>(checks), static_cast<unsigned long long>(mismatches));
}
#endif
// Appends next to previous when it directly follows it in both arenas and the
// combined draw stays within 16-bit indices: one triangle list, same order.
bool AppendGeometry(AuroraNativeMaterialGeometry& previous, const AuroraNativeMaterialGeometry& next) {
    if (previous.vertices + previous.vertex_count != next.vertices || previous.indices + previous.index_count != next.indices || previous.vertex_count + next.vertex_count > 65536)
        return false;

    const auto base = uint16_t(previous.vertex_count);

    for (uint32_t i = 0; i < next.index_count; ++i)
        next.indices[i] = uint16_t(next.indices[i] + base);

    previous.vertex_count += next.vertex_count;
    previous.index_count += next.index_count;
    return true;
}

Feedback& PushFeedback(Kind kind) {
    RequireRecorder();
    auto& p = *recording;

    if (p.feedbackCount == p.feedback.size())
        p.feedback.emplace_back();

    p.records.push_back({kind, false, p.feedbackCount, {}});
    return p.feedback[p.feedbackCount++];
}

template <class Geometry>
bool Allocate(uint32_t vertices, uint32_t indices, Geometry& out) {
    out = {};

    if (!NativeFramePacketRecording() || !vertices || !indices)
        return false;

    auto& p = *recording;
    const size_t start = (p.vertexCursor + 15) & ~size_t(15);
    const size_t vertexBytes = size_t(vertices) * sizeof(*out.vertices), indexBytes = (size_t(indices) * 2 + 3) & ~size_t(3);

    if (vertices > 65536 || start + vertexBytes > VertexArenaBytes || p.indexCursor + indexBytes > IndexArenaBytes) {
        std::fprintf(stderr, "MH3 frame packet: geometry arenas full, frame %llu canceled\n", static_cast<unsigned long long>(p.sequence));
        p.canceled = true;
        return false;
    }

    out.vertices = reinterpret_cast<decltype(out.vertices)>(p.vertices.get() + start);
    out.indices = reinterpret_cast<uint16_t*>(p.indices.get() + p.indexCursor);
    out.vertex_count = vertices;
    out.index_count = indices;
    out.frame = uint32_t(p.sequence);
    out.slot = p.slot;
    p.vertexCursor = start + vertexBytes;
    p.indexCursor += indexBytes;
    return true;
}

// ---- Render thread ----
void RunTasks(std::deque<Task>& queue, uint64_t through) {
    for ([[maybe_unused]] uint32_t index = 0;; ++index) {
        std::function<void()> run;
        {
            std::lock_guard lock(taskLock);

            if (queue.empty() || queue.front().sequence > through)
                return;

            run = std::move(queue.front().run);
            queue.pop_front();
        }
#ifdef MH3_DEVELOPMENT
        renderStep.store(index, std::memory_order_relaxed);
#endif
        run();
    }
}

void Restore(const Packet& p, const StateIndices& state, StateIndices& applied) {
    // Any restore except lighting replaces state the deferred model
    // preparation's caches describe.
    bool forget = false;

    // Draw state first: the material snapshot already holds its raster import.
    if (applied[NATIVE_STATE_DRAW] != state[NATIVE_STATE_DRAW]) {
        RestoreNativeDrawSnapshot(p.draws.items[state[NATIVE_STATE_DRAW]]);
        forget = true;
    }

    if (applied[NATIVE_STATE_LIGHTING] != state[NATIVE_STATE_LIGHTING])
        RestoreNativeLightingSnapshot(p.lighting.items[state[NATIVE_STATE_LIGHTING]]);

    if (applied[NATIVE_STATE_POST] != state[NATIVE_STATE_POST]) {
        RestoreNativePostSnapshot(p.posts.items[state[NATIVE_STATE_POST]]);
        forget = true;
    }

    if (applied[NATIVE_STATE_TEXTURES] != state[NATIVE_STATE_TEXTURES]) {
        RestoreNativeTextureSnapshot(p.textures.items[state[NATIVE_STATE_TEXTURES]]);
        forget = true;
    }

    bool material = false;

    if (applied[NATIVE_STATE_MATERIAL] != state[NATIVE_STATE_MATERIAL]) {
        RestoreNativeMaterialSnapshot(p.materials.items[state[NATIVE_STATE_MATERIAL]]);
        forget = material = true;
    }

    // The colour values replace the material snapshot's copies of them.
    if (material || applied[NATIVE_STATE_COLORS] != state[NATIVE_STATE_COLORS]) {
        RestoreNativeColorSnapshot(p.colors.items[state[NATIVE_STATE_COLORS]]);
        forget = true;
    }

    applied = state;

    if (forget)
        ForgetNativeModelPrepCaches();
}
#ifdef MH3_DEVELOPMENT
// MH3_VERIFY=replay: the render thread's material and colours, as Restore
// left them, against the game thread's snapshots for this draw.
void VerifyReplayedState(const Packet& p, const StateIndices& state, uint32_t record) {
    static const bool enabled = VerifyOptions().Has("replay");

    if (!enabled)
        return;

    static uint32_t checks = 0, mismatches = 0;
    NativeMaterialSnapshot expected = p.materials.items[state[NATIVE_STATE_MATERIAL]];
    ApplyNativeColorSnapshot(expected.material, p.colors.items[state[NATIVE_STATE_COLORS]]);
    const auto& actual = GetNativeMaterialState(NativeOwner::Render);
    ++checks;

    if (!BytesEqual(&actual, &expected.material, sizeof actual)) {
        const bool colours = std::memcmp(actual.color, expected.material.color, sizeof actual.color) || std::memcmp(actual.konst, expected.material.konst, sizeof actual.konst);

        if (++mismatches <= 20)
            std::fprintf(stderr, "MH3 replay check: record %u (after %u) draws with another material%s\n", record, record ? unsigned(p.records[record - 1].kind) : 0u,
                         colours ? " and other colours" : "");
    }

    if ((checks & 0xFFFF) == 0)
        std::fprintf(stderr, "MH3 replay check: %u of %u material geometry draws mismatched\n", mismatches, checks);
}
#endif
#ifdef __vita__
// GXM reads vertices and indices in place when it executes the frame, so the
// packet's heap arenas cannot be drawn from: copy each draw into the
// renderer's GPU-mapped frame slot, which is fenced until the GPU is done.
template <class Geometry>
bool GpuGeometry(const Geometry& source, Geometry& out, int (*allocate)(uint32_t, uint32_t, Geometry*)) {
    if (!allocate(source.vertex_count, source.index_count, &out))
        return false;

    std::memcpy(out.vertices, source.vertices, size_t(source.vertex_count) * sizeof(*source.vertices));
    std::memcpy(out.indices, source.indices, size_t(source.index_count) * sizeof(uint16_t));
    return true;
}
#endif
#ifdef MH3_DEVELOPMENT
// MH3_PROFILE=skip=KIND[+KIND...]: the render thread drops these draw records
// (model, material: material geometry such as effects, geometry: the other
// pre-transformed geometry) or meshes of model draws: skinned ones (skinned),
// those whose material blends (blended) or alpha tests (discard), to
// attribute GPU time to them.
uint32_t skipMeshes = 0;

uint32_t SkippedKinds() {
    static const uint32_t mask = [] {
        uint32_t kinds = 0;
        const char* value = ProfileOptions().Value("skip");

        for (std::string_view rest = value ? value : ""; !rest.empty();) {
            const size_t plus = rest.find('+');
            const std::string_view kind = rest.substr(0, plus);

            if (kind == "model")
                kinds |= 1u << unsigned(Kind::Model);
            else if (kind == "material")
                kinds |= 1u << unsigned(Kind::MaterialGeometry);
            else if (kind == "geometry")
                kinds |= 1u << unsigned(Kind::Geometry);
            else if (kind == "skinned")
                skipMeshes |= NATIVE_SKIP_SKINNED;
            else if (kind == "blended")
                skipMeshes |= NATIVE_SKIP_BLENDED;
            else if (kind == "discard")
                skipMeshes |= NATIVE_SKIP_DISCARD;
            else
                std::fprintf(stderr, "MH3: MH3_PROFILE: skip=%.*s is not model, material, geometry, skinned, blended or discard\n", int(kind.size()), kind.data());

            rest = plus == std::string_view::npos ? std::string_view{} : rest.substr(plus + 1);
        }

        return kinds;
    }();
    return mask;
}
#endif
// Whether a record reads the stored depth: a draw whose depth test can both
// pass and fail. A model draw publishes its materials' rasters as it draws, so
// it counts as reading.
bool ReadsDepth(const Packet& p, const Record& record) {
    if (record.kind == Kind::Model)
        return true;

    if (record.kind != Kind::MaterialGeometry && record.kind != Kind::Geometry)
        return false;

    const auto& snapshot = p.draws.items[record.state[NATIVE_STATE_DRAW]];
    const auto& raster = snapshot.draw.raster;
    return !snapshot.rasterKnown || (raster.depth_test && raster.cull_mode != 3 && raster.depth_compare != 0 && raster.depth_compare != 7);
}

// Feedback operations render to other targets: the frame target's scene ends.
bool EndsTargetScene(Kind kind) {
    return kind == Kind::Capture || kind == Kind::Glare || kind == Kind::Clone;
}

// Depth liveness for the renderer from record `from` on: whether a record at
// or after it reads depth, and whether one does after the next feedback
// operation ends the frame target's scene.
void PublishDepthLiveness(const Packet& p, GLuint depth, uint32_t from, uint32_t lastReader) {
    if (lastReader == UINT32_MAX || from > lastReader) {
        aurora::gl::set_depth_liveness(depth, false, false);
        return;
    }

    bool after = false;

    for (uint32_t i = from; i < lastReader && !after; ++i)
        after = EndsTargetScene(p.records[i].kind);

    aurora::gl::set_depth_liveness(depth, true, after);
}

struct DepthLivenessScope {
    ~DepthLivenessScope() { aurora::gl::set_depth_liveness(0, true, true); }
};

void Draw(const Packet& p) {
    const auto& color = aurora::gpu::g_frameBuffer;
    const auto& depth = aurora::gpu::g_depthBuffer;
    aurora::gl::bind_framebuffer(color.texture->gl(), depth.texture->gl());
    uint32_t lastReader = UINT32_MAX;

    for (uint32_t i = 0; i < p.records.size(); ++i)
        if (ReadsDepth(p, p.records[i]))
            lastReader = i;
    const GLuint depthTexture = depth.texture->gl();
    DepthLivenessScope livenessScope;
    PublishDepthLiveness(p, depthTexture, 0, lastReader);
    aurora::gl::set_viewport(0, 0, color.width, color.height, 0.f, 1.f);
    aurora::gl::set_scissor_enabled(false);
    aurora::gl::set_color_mask(true, true, true, true);
    aurora::gl::set_depth_write(true);

    // Depth first: on the Vita a scene that starts with a full depth clear
    // takes it from the depth surface's background instead of loading it.
    aurora::gl::clear_depth(0.f); // native shader projection uses reversed Z
    const float black[4]{0.f, 0.f, 0.f, 1.f};
    aurora::gl::clear_color(black);
    StateIndices applied;
    const auto forget = [&] {
        applied.fill(UINT32_MAX);
        ForgetNativeTextureBindings();
        ForgetNativeModelPrepCaches();
    };

    forget();
    bool afterModel = false;

    for (uint32_t index = 0; const auto& record : p.records) {
        MH3_RENDER_STAGE(RenderStage::Draw, index << 8 | uint32_t(record.kind));
        MH3_RENDER_PART(Feedback);

        if (!aurora_native_frame_should_render())
            return;
        // Past the last depth reader, or a new frame target scene ahead.
        if ((lastReader != UINT32_MAX && index == lastReader + 1) || (index && index <= lastReader && lastReader != UINT32_MAX && EndsTargetScene(p.records[index - 1].kind)))
            PublishDepthLiveness(p, depthTexture, index, lastReader);

        ++index;

        switch (record.kind) {
        case Kind::Capture: {
            const auto& op = p.feedback[record.payload];
            aurora::publish_scene_capture(op.identity, op.capture);
            forget();
            continue;
        }
        case Kind::Glare: {
            const auto& op = p.feedback[record.payload];
            aurora::publish_scene_glare(op.identity, op.glare);
            forget();
            continue;
        }
        case Kind::Clone: {
            const auto& op = p.feedback[record.payload];

            if (!aurora::clone_scene_feedback_texture(op.identity, op.source))
                MH3_THROW(std::runtime_error("Scene feedback clone source was not captured"));

            forget();
            continue;
        }
        default:
            break;
        }
#ifdef MH3_DEVELOPMENT
        if (SkippedKinds() >> unsigned(record.kind) & 1)
            continue;
#endif
        MH3_RENDER_PART(Restore);

        // A model item prepared here (deferred) set the render thread's draw,
        // material, post-matrix, texture and colour state for itself. The next
        // draw that is not a model restores them even where its snapshots are
        // the ones applied before the model: the game thread's copies did not
        // change, so their indices did not either. Without this, effect
        // particles drawn after a model in a sorted bucket took its material
        // or colours (steam red for a frame, particles drawn as solid quads).
        if (afterModel && record.kind != Kind::Model) {
            for (const unsigned component : {NATIVE_STATE_DRAW, NATIVE_STATE_MATERIAL, NATIVE_STATE_POST, NATIVE_STATE_TEXTURES, NATIVE_STATE_COLORS})
                applied[component] = UINT32_MAX;

            afterModel = false;
        }

        Restore(p, record.state, applied);
#ifdef MH3_DEVELOPMENT
        if (record.kind == Kind::MaterialGeometry)
            VerifyReplayedState(p, record.state, index - 1);
#endif
        switch (record.kind) {
        case Kind::Model:
            MH3_RENDER_PART(ModelPrep);
            ExecuteNativeModelDraw(p.slot, record.payload);
            afterModel = true;
            break;
        case Kind::MaterialGeometry: {
            MH3_RENDER_PART(MaterialGeometry);
#ifdef __vita__
            AuroraNativeMaterialGeometry geometry;

            if (GpuGeometry(p.materialGeometry[record.payload], geometry, aurora_allocate_native_material_geometry))
                DrawNativeMaterialGeometry(geometry);
#else
            DrawNativeMaterialGeometry(p.materialGeometry[record.payload]);
#endif
            break;
        }
        case Kind::Quads: {
            MH3_RENDER_PART(MaterialGeometry);
            const auto& quads = p.quads[record.payload];
            DrawNativeQuads(quads.vertices, quads.count, quads.normal, quads.color);
            break;
        }
        case Kind::Geometry: {
            MH3_RENDER_PART(Geometry);
#ifdef __vita__
            AuroraNativeGeometry geometry;

            if (GpuGeometry(p.geometry[record.payload], geometry, aurora_allocate_native_geometry))
                aurora::gl::draw_native_geometry(geometry, record.textured);
#else
            aurora::gl::draw_native_geometry(p.geometry[record.payload], record.textured);
#endif
            break;
        }
        case Kind::DepthClear:
            MH3_RENDER_PART(Geometry);
            aurora::gl::clear_depth_full_target(0.f);
            break;
        default:
            break;
        }
    }
}

void Execute(const Packet& p) {
#ifdef MH3_DEVELOPMENT
    renderPacket.store(p.sequence, std::memory_order_relaxed);
#endif
    MH3_RENDER_STAGE(RenderStage::BeforeTasks);
    MH3_RENDER_PART(Tasks);
    RunTasks(beforeTasks, p.sequence);

    if (p.render && !p.canceled) {
        MH3_RENDER_STAGE(RenderStage::Begin);
        MH3_RENDER_PART(Begin);

        if (aurora_begin_native_frame())
            Draw(p);

        if (!p.capture.empty())
            aurora_request_frame_capture(aurora::gfx::current_frame() + 1, p.capture.c_str());

        MH3_RENDER_STAGE(RenderStage::End);
        MH3_RENDER_PART(End);
#ifdef MH3_DEVELOPMENT
        renderSplitPackets.fetch_add(renderSplit, std::memory_order_relaxed);
#endif
#ifdef MH3_DEVELOPMENT
        aurora::gl::set_present_timing({p.gameUs, p.inputNs / 1000, p.publishedNs / 1000});
#endif
        aurora_end_native_frame();
    }

    MH3_RENDER_STAGE(RenderStage::AfterTasks);
    MH3_RENDER_PART(Tasks);
    RunTasks(afterTasks, p.sequence);
    MH3_RENDER_STAGE(RenderStage::Waiting);
    MH3_RENDER_PART(Waiting);
}

// Waits until a packet after `ticket` is published (or the thread stops,
// which also publishes). Packets come in bursts: SpinSignal spins for a
// millisecond, then sleeps on its condition variable.
void WaitForPacket(uint32_t ticket) {
    published.AwaitChange(ticket);
}

// Replays published packets until the thread stops, then runs the resource
// work queued after the last one.
#ifdef MH3_MEMCPY_CENSUS
extern "C" void mh3_memcpy_census_start();
extern "C" int mh3_memcpy_census_take(uintptr_t* callers, uint64_t* bytes, uint32_t* calls, int max);

// The render thread's copies by calling place every 300 packets, the most
// bytes first (runtime/vita/aeabi_memory.c; symbolize the callers with
// llvm-symbolizer on the ELF).
void ReportMemcpyCensus() {
    constexpr int Max = 1024;
    static uintptr_t callers[Max];
    static uint64_t bytes[Max];
    static uint32_t calls[Max];
    const int count = mh3_memcpy_census_take(callers, bytes, calls, Max);
    int order[Max];
    uint64_t total = 0;

    for (int i = 0; i < count; ++i) {
        order[i] = i;
        total += bytes[i];
    }

    std::sort(order, order + count, [](int a, int b) { return bytes[a] > bytes[b]; });
    std::fprintf(stderr, "MH3 memcpy census: %.1f KiB over 300 packets from %d places\n", double(total) / 1024.0, count);

    for (int k = 0; k < std::min(count, 16); ++k)
        std::fprintf(stderr, "MH3 memcpy census: 0x%08x %.1f KiB %u calls\n", unsigned(callers[order[k]]), double(bytes[order[k]]) / 1024.0, calls[order[k]]);
}
#endif

// The game thread limiter on: a packet that presents waits here for a free
// GPU frame slot (and display queue entry) instead of skipping its draw, so
// the render thread consumes packets as fast as it submits frames and the game
// thread, waiting for consumption, steps once per frame drawn. A GPU stuck for
// 100 ms falls back to the skip.
void WaitForGpuFrameSlot() {
    const auto start = std::chrono::steady_clock::now();

    while (aurora::gl::gpu_frame_pending() && !stopping.load(std::memory_order_relaxed)) {
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(100))
            return;
#ifdef __vita__
        sceKernelDelayThread(200);
#else
        std::this_thread::sleep_for(std::chrono::microseconds(200));
#endif
    }
}

void RenderPackets() {
#ifdef __vita__
    // The game thread runs on core 2; the AX mix worker and the display
    // callback on core 0.
    if (sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_1) < 0)
        MH3_THROW(std::runtime_error("Render thread core 1 affinity failed"));
#endif
#ifdef MH3_MEMCPY_CENSUS
    mh3_memcpy_census_start();
    uint64_t censusPackets = 0;
#endif
#ifdef MH3_DEVELOPMENT
    uint64_t replayed = 0, drawPackets = 0, wallNs = 0, drawWallNs = 0, waitNs = 0, waits = 0, idleStart = 0, wakeNs = 0, wakes = 0, lateWakes = 0;
#ifdef __vita__
    renderCounters.thread.store(sceKernelGetThreadId(), std::memory_order_release);
#elif defined(__linux__)
    clockid_t cpuClock;

    if (pthread_getcpuclockid(pthread_self(), &cpuClock) == 0)
        renderCounters.cpuClock.store(cpuClock, std::memory_order_release);

    renderCounters.thread.store(int32_t(syscall(SYS_gettid)), std::memory_order_release);
#endif
#endif
    for (;;) {
        const uint32_t ticket = published.Load();
        const uint64_t newest = lastPublished.load(std::memory_order_acquire), current = taken.load(std::memory_order_relaxed);

        if (newest <= current) {
            if (stopping.load(std::memory_order_acquire))
                break;
#ifdef MH3_DEVELOPMENT
            if (!idleStart)
                idleStart = RenderMonotonicNs();
#endif
            WaitForPacket(ticket);
            continue;
        }

        // In order; without the limiter the newest, skipping the older ones
        // (their tasks run with it). Taken before its slot is read: the game
        // thread reuses a skipped packet's slot once a newer one is taken.
        const uint64_t next = paced || synchronous ? current + 1 : newest;
        taken.store(next, std::memory_order_release);
        takenSignal.Advance();
        const auto& p = packets[next % kNativeFramePacketSlots];

        if (p.sequence != next)
            MH3_THROW(std::runtime_error("Frame packet ring out of order"));
#ifdef MH3_DEVELOPMENT
        const auto replayStart = RenderMonotonicNs();

        if (idleStart) {
            waitNs += replayStart - idleStart;
            ++waits;

            if (p.publishedNs > idleStart) {
                const uint64_t wake = replayStart - p.publishedNs;
                wakeNs += wake;
                ++wakes;
                lateWakes += wake > 1000000;
            }

            idleStart = 0;
        }
#endif
        if (paced && !synchronous && p.render && !p.canceled)
            WaitForGpuFrameSlot();

        Execute(p);

        // Its quads were drawn from the packet's arena (SlotFree).
        if (!p.quads.empty())
            packets[p.sequence % kNativeFramePacketSlots].gpuDone = aurora::gl::submitted_gpu_fence();
#ifdef MH3_MEMCPY_CENSUS
        if (++censusPackets % 300 == 0)
            ReportMemcpyCensus();
#endif
#ifdef MH3_DEVELOPMENT
        const auto elapsed = RenderMonotonicNs() - replayStart;
        ++replayed;
        wallNs += elapsed;

        if (p.render && !p.canceled) {
            ++drawPackets;
            drawWallNs += elapsed;
        }

        // Atomic payloads keep the bounded sequence snapshot race-free.
        // Only this render owner writes them, once per completed packet.
        renderCounters.revision.fetch_add(1, std::memory_order_acq_rel);
        renderCounters.packets.store(replayed, std::memory_order_relaxed);
        renderCounters.drawPackets.store(drawPackets, std::memory_order_relaxed);
        renderCounters.wallNs.store(wallNs, std::memory_order_relaxed);
        renderCounters.drawWallNs.store(drawWallNs, std::memory_order_relaxed);
        renderCounters.waitNs.store(waitNs, std::memory_order_relaxed);
        renderCounters.waits.store(waits, std::memory_order_relaxed);
        renderCounters.wakeNs.store(wakeNs, std::memory_order_relaxed);
        renderCounters.wakes.store(wakes, std::memory_order_relaxed);
        renderCounters.lateWakes.store(lateWakes, std::memory_order_relaxed);
        renderCounters.revision.fetch_add(1, std::memory_order_release);
#endif
        completed.store(p.sequence, std::memory_order_release);
        completed.notify_all();
        takenSignal.Advance();
    }

    // Resource work queued after the last packet.
    MH3_RENDER_STAGE(RenderStage::StopTasks);
    RunTasks(beforeTasks, UINT64_MAX);
    RunTasks(afterTasks, UINT64_MAX);
}

void RenderLoop() {
    aurora::gl::acquire_context();
#if defined(__cpp_exceptions)
    // A failure goes to the game thread, which reports it.
    try {
        RenderPackets();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "MH3 render thread: %s\n", error.what());
        failure = std::current_exception();
        failed.store(true, std::memory_order_release);
        takenSignal.Advance();
    } catch (...) {
        failure = std::current_exception();
        failed.store(true, std::memory_order_release);
        takenSignal.Advance();
    }
#else
    RenderPackets();
#endif

    completed.store(UINT64_MAX, std::memory_order_release);
    completed.notify_all();
#ifdef MH3_DEVELOPMENT
    renderCounters.thread.store(-1, std::memory_order_release);
#endif
    aurora::gl::release_context();
}

#ifdef __EMSCRIPTEN__
// The web build (docs/web-build.md): WebGPU belongs to the browser's main
// thread, which shows a frame only once it returns to its event loop. The
// render thread is a requestAnimationFrame loop there: each animation frame
// replays the next published packet, as RenderPackets would, and its present
// appears when the callback returns.
std::atomic<bool> webLoopRunning{false};

void FinishWebRenderLoop() {
    completed.store(UINT64_MAX, std::memory_order_release);
    completed.notify_all();
    aurora::gl::release_context();
    webLoopRunning.store(false, std::memory_order_release);
    webLoopRunning.notify_all();
}

EM_BOOL RenderAnimationFrame(double, void*) {
#if defined(__cpp_exceptions)
    // A failure goes to the game thread, which reports it.
    try {
#endif
        const uint64_t newest = lastPublished.load(std::memory_order_acquire), current = taken.load(std::memory_order_relaxed);

        if (newest > current) {
            const uint64_t next = paced || synchronous ? current + 1 : newest;
            taken.store(next, std::memory_order_release);
            takenSignal.Advance();
            const auto& p = packets[next % kNativeFramePacketSlots];

            if (p.sequence != next)
                MH3_THROW(std::runtime_error("Frame packet ring out of order"));

            Execute(p);
            completed.store(p.sequence, std::memory_order_release);
            completed.notify_all();
            takenSignal.Advance();
            return EM_TRUE;
        }

        if (!stopping.load(std::memory_order_acquire))
            return EM_TRUE;

        // Resource work queued after the last packet.
        RunTasks(beforeTasks, UINT64_MAX);
        RunTasks(afterTasks, UINT64_MAX);
#if defined(__cpp_exceptions)
    } catch (const std::exception& error) {
        std::fprintf(stderr, "MH3 render loop: %s\n", error.what());
        failure = std::current_exception();
        failed.store(true, std::memory_order_release);
        takenSignal.Advance();
    } catch (...) {
        failure = std::current_exception();
        failed.store(true, std::memory_order_release);
        takenSignal.Advance();
    }
#endif
    FinishWebRenderLoop();
    return EM_FALSE;
}

void StartWebRenderLoop(void*) {
    aurora::gl::acquire_context();
    emscripten_request_animation_frame_loop(RenderAnimationFrame, nullptr);
}
#endif

// A publish that reads back (captures) or runs synchronously waits for its
// packet here. Short sleeps that re-check, not std::atomic::wait: on the
// Vita that parks on a pthread condition variable, which can lose the wakeup
// (see WaitForPacket) and would hold the game thread for good.
void WaitCompleted(uint64_t target) {
    while (completed.load(std::memory_order_acquire) < target) {
#ifdef __vita__
        sceKernelDelayThread(100);
#else
        std::this_thread::sleep_for(std::chrono::microseconds(100));
#endif
    }

    RethrowFailure();
}
} // namespace

bool OnNativeGraphicsThread() {
    return aurora::gl::on_render_thread() || (running.load(std::memory_order_relaxed) && OnProducer());
}

bool AllocateNativeGeometry(uint32_t vertices, uint32_t indices, AuroraNativeGeometry& out) {
    return Allocate(vertices, indices, out);
}

bool AllocateNativeMaterialGeometry(uint32_t vertices, uint32_t indices, AuroraNativeMaterialGeometry& out) {
    return Allocate(vertices, indices, out);
}

AuroraNativeQuadVertex* AllocateNativeQuads(uint32_t count) {
#ifdef __EMSCRIPTEN__
    (void)count;
    return nullptr;
#else
    if (!NativeFramePacketRecording() || !count)
        return nullptr;

    auto& p = *recording;

    if (!p.quadArena) {
        p.quadArena = static_cast<AuroraNativeQuadVertex*>(aurora_map_native_quad_memory(QuadArenaVertices * sizeof(AuroraNativeQuadVertex)));

        if (!p.quadArena)
            return nullptr;
    }

    // Full: the caller takes the full material geometry.
    if (p.quadCursor + 4 * count > QuadArenaVertices)
        return nullptr;

    auto* out = p.quadArena + p.quadCursor;
    p.quadCursor += 4 * count;
    return out;
#endif
}

void RecordNativeQuads(AuroraNativeQuadVertex* vertices, uint32_t count, const float normal[3], const uint8_t color[4]) {
    RequireRecorder();

    if (!NativeFramePacketRecording())
        return;

    auto& p = *recording;
    const StateIndices state = CaptureState(false);
#ifdef MH3_DEVELOPMENT
    ++geometrySubmitted;
#endif
    // Batching as for material geometry; the normal and colour come from the
    // same draw and material state, and the quads follow on in the arena.
    if (!p.records.empty()) {
        const auto& last = p.records.back();

        if (last.kind == Kind::Quads && last.state[NATIVE_STATE_DRAW] == state[NATIVE_STATE_DRAW] && last.state[NATIVE_STATE_MATERIAL] == state[NATIVE_STATE_MATERIAL] &&
            last.state[NATIVE_STATE_LIGHTING] == state[NATIVE_STATE_LIGHTING] && last.state[NATIVE_STATE_TEXTURES] == state[NATIVE_STATE_TEXTURES] &&
            last.state[NATIVE_STATE_COLORS] == state[NATIVE_STATE_COLORS]) {
            auto& previous = p.quads[last.payload];

            if (previous.vertices + 4 * previous.count == vertices && previous.count + count <= aurora::gl::MaxNativeQuads &&
                !std::memcmp(previous.normal, normal, sizeof previous.normal) && !std::memcmp(previous.color, color, sizeof previous.color)) {
                previous.count += count;
#ifdef MH3_DEVELOPMENT
                ++geometryMerged;
#endif
                return;
            }
        }
    }

    Quads quads{vertices, count, {normal[0], normal[1], normal[2]}, {color[0], color[1], color[2], color[3]}};
    p.quads.push_back(quads);
    p.records.push_back({Kind::Quads, false, uint32_t(p.quads.size() - 1), state});
}

void RecordNativeMaterialGeometry(const AuroraNativeMaterialGeometry& geometry) {
    RequireRecorder();

    if (!NativeFramePacketRecording())
        return;

    auto& p = *recording;
    const StateIndices state = CaptureState(false);
#ifdef MH3_DEVELOPMENT
    ++geometrySubmitted;
    CheckBatchedState(p, state);
#endif
    // Batching: geometry drawn with exactly the state of the draw before it
    // (the same draw, material, colour, lighting and texture snapshots; its post
    // matrices are not read) joins that draw. Primitive order within one draw
    // is submission order, so blending and depth results are unchanged.
    if (!p.records.empty()) {
        const auto& last = p.records.back();
#ifdef MH3_DEVELOPMENT
        if (last.kind != Kind::MaterialGeometry)
            ++geometryUnmerged[0];
        else {
            const unsigned components[5] = {NATIVE_STATE_DRAW, NATIVE_STATE_MATERIAL, NATIVE_STATE_LIGHTING, NATIVE_STATE_TEXTURES, NATIVE_STATE_COLORS};

            for (unsigned i = 0; i < 5; ++i)
                if (last.state[components[i]] != state[components[i]])
                    ++geometryUnmerged[1 + i];
        }
#endif
        if (last.kind == Kind::MaterialGeometry && last.state[NATIVE_STATE_DRAW] == state[NATIVE_STATE_DRAW] && last.state[NATIVE_STATE_MATERIAL] == state[NATIVE_STATE_MATERIAL] &&
            last.state[NATIVE_STATE_LIGHTING] == state[NATIVE_STATE_LIGHTING] && last.state[NATIVE_STATE_TEXTURES] == state[NATIVE_STATE_TEXTURES] &&
            last.state[NATIVE_STATE_COLORS] == state[NATIVE_STATE_COLORS] &&
            AppendGeometry(p.materialGeometry[last.payload], geometry)) {
#ifdef MH3_DEVELOPMENT
            ++geometryMerged;
#endif
            return;
        }
    }

    p.materialGeometry.push_back(geometry);
    p.records.push_back({Kind::MaterialGeometry, false, uint32_t(p.materialGeometry.size() - 1), state});
}

void SubmitNativeGeometry(const AuroraNativeGeometry& geometry, bool textured) {
    if (!NativeFramePacketRecording() || !geometry.index_count)
        return;

    recording->geometry.push_back(geometry);
    Push(Kind::Geometry, uint32_t(recording->geometry.size() - 1), textured);
}

void RecordNativeModelDraw(uint32_t item, bool deferred) {
    if (!NativeFramePacketRecording())
        return;

    Push(Kind::Model, item);

    if (deferred) {
        // The render thread prepares this item into its draw, material,
        // texture and post-matrix state (lighting stays the game's).
        auto& p = *recording;
        p.draws.diverged = p.materials.diverged = p.posts.diverged = p.textures.diverged = p.colors.diverged = true;
    }
}

void RecordNativeDepthClear() {
    if (NativeFramePacketRecording())
        Push(Kind::DepthClear, 0);
}

void RecordNativeSceneCapture(const void* identity, const aurora::SceneCaptureParameters& parameters) {
    if (!NativeFramePacketRecording())
        return;

    if (!identity)
        MH3_THROW(std::runtime_error("Scene feedback has no producer identity"));

    auto& op = PushFeedback(Kind::Capture);
    op.identity = identity;
    op.capture = parameters;
    feedbackTargets[identity] = {parameters.width, parameters.height, ++feedbackRevision};
}

void RecordNativeSceneGlare(const void* identity, const aurora::SceneGlareParameters& parameters) {
    if (!NativeFramePacketRecording())
        return;

    if (!identity)
        MH3_THROW(std::runtime_error("Scene feedback has no producer identity"));

    auto& op = PushFeedback(Kind::Glare);
    op.identity = identity;
    op.glare = parameters;
    feedbackTargets[identity] = {parameters.width, parameters.height, ++feedbackRevision};
}

bool RecordNativeSceneClone(const void* destination, const void* source) {
    if (!NativeFramePacketRecording())
        return false;

    const auto found = feedbackTargets.find(source);

    if (found == feedbackTargets.end())
        return false;

    auto& op = PushFeedback(Kind::Clone);
    op.identity = destination;
    op.source = source;
    feedbackTargets[destination] = {found->second.width, found->second.height, ++feedbackRevision};
    return true;
}

uint64_t NativeSceneFeedbackVersion(const void* identity, uint32_t width, uint32_t height) {
    const auto found = feedbackTargets.find(identity);
    return found != feedbackTargets.end() && found->second.width == width && found->second.height == height ? found->second.revision : 0;
}

void QueueNativeGraphicsTask(NativeTaskOrder order, std::function<void()> task) {
    if (!running.load(std::memory_order_acquire)) {
        task();
        return;
    }

    RequireGame();
    std::lock_guard lock(taskLock);
    (order == NativeTaskOrder::BeforeDraws ? beforeTasks : afterTasks).push_back({sequence, std::move(task)});
}

uint32_t NativeFramePacketSlot() {
    if (!recording)
        MH3_THROW(std::runtime_error("No frame packet is being recorded"));

    return recording->slot;
}

uint64_t NativeFramePacketSequence() {
    return sequence;
}

bool FrameLimitSetting() {
    return frameLimit;
}

void SetFrameLimitSetting(bool limit) {
    frameLimit = limit;
}

uint64_t NativeFramePacketsDone() {
    // The render thread takes packets in sequence order: once it finished
    // one, every older packet has rendered or was skipped and is never read.
    return running.load(std::memory_order_acquire) ? completed.load(std::memory_order_acquire) : UINT64_MAX;
}

void StartNativeRenderThread() {
    if (running)
        MH3_THROW(std::runtime_error("Render thread already started"));

    if (!aurora::gl::on_render_thread())
        MH3_THROW(std::runtime_error("Render thread started outside the graphics owner"));

    for (uint32_t i = 0; i < packets.size(); ++i)
        packets[i].slot = i;

    paced = frameLimit;
#ifdef MH3_DEVELOPMENT
    renderCounters.thread.store(-1, std::memory_order_relaxed);
    renderCounters.revision.store(0, std::memory_order_relaxed);
    renderCounters.packets.store(0, std::memory_order_relaxed);
    renderCounters.drawPackets.store(0, std::memory_order_relaxed);
    renderCounters.wallNs.store(0, std::memory_order_relaxed);
    renderCounters.drawWallNs.store(0, std::memory_order_relaxed);
#ifdef __linux__
    renderCounters.cpuClock.store(-1, std::memory_order_relaxed);
#endif
    // MH3_PROFILE=sync: wait for every packet to render (serial frames).
    synchronous = ProfileOptions().Has("sync");
    renderSplit = ProfileOptions().Has("render");

    if (static bool sampling = false; renderSplit && !sampling) {
        std::thread(SampleRenderParts).detach();
        sampling = true;
    }
#endif
    gameThread = std::this_thread::get_id();
    stopping = false;
    failed = false;

    // A restart (the launcher's renderer, then the game's) continues the
    // packet sequence; the previous thread left completed at its maximum.
    completed.store(sequence, std::memory_order_relaxed);
    taken.store(sequence, std::memory_order_relaxed);
    lastPublished.store(sequence, std::memory_order_relaxed);
    aurora::gl::release_context();
    running = true;
#ifdef __EMSCRIPTEN__
    webLoopRunning.store(true, std::memory_order_release);
    emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), StartWebRenderLoop, nullptr);
#else
    renderThread = std::thread(RenderLoop);
#endif
}

void StopNativeRenderThread() {
    if (!running)
        return;

    stopping.store(true, std::memory_order_release);
    published.Advance();
#ifdef __EMSCRIPTEN__
    for (bool loop = webLoopRunning.load(std::memory_order_acquire); loop; loop = webLoopRunning.load(std::memory_order_acquire))
        webLoopRunning.wait(true, std::memory_order_acquire);
#else
    renderThread.join();
#endif
    running = false;
    recording = nullptr;
    aurora::gl::acquire_context();

    // A failed render thread leaves its queued work behind.
    RunTasks(beforeTasks, UINT64_MAX);
    RunTasks(afterTasks, UINT64_MAX);
}

bool NativeRenderThreadRunning() {
    return running.load(std::memory_order_acquire);
}
#ifdef MH3_DEVELOPMENT
NativeRenderThreadStatistics ReadNativeRenderThreadStatistics() noexcept {
    NativeRenderThreadStatistics result;
    const auto thread = renderCounters.thread.load(std::memory_order_acquire);
    result.cpu = RenderCpuSample(thread);
    result.sampled_at_ns = RenderMonotonicNs();

    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const auto before = renderCounters.revision.load(std::memory_order_acquire);

        if (before & 1)
            continue;

        result.packets = renderCounters.packets.load(std::memory_order_relaxed);
        result.draw_packets = renderCounters.drawPackets.load(std::memory_order_relaxed);
        result.packet_wall_ns = renderCounters.wallNs.load(std::memory_order_relaxed);
        result.draw_packet_wall_ns = renderCounters.drawWallNs.load(std::memory_order_relaxed);
        result.packet_wait_ns = renderCounters.waitNs.load(std::memory_order_relaxed);
        result.packet_waits = renderCounters.waits.load(std::memory_order_relaxed);
        result.wake_ns = renderCounters.wakeNs.load(std::memory_order_relaxed);
        result.wakes = renderCounters.wakes.load(std::memory_order_relaxed);
        result.late_wakes = renderCounters.lateWakes.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);

        if (before == renderCounters.revision.load(std::memory_order_relaxed)) {
            result.counters_available = thread >= 0;
            break;
        }
    }

    return result;
}

void MarkNativeRenderModelDraw() {
    MH3_RENDER_PART(ModelDraw);
}

NativeRenderPartScope::NativeRenderPartScope(unsigned part) noexcept : previous_(renderPart.load(std::memory_order_relaxed)) {
    if (renderSplit)
        renderPart.store(part, std::memory_order_relaxed);
}

NativeRenderPartScope::~NativeRenderPartScope() {
    if (renderSplit)
        renderPart.store(previous_, std::memory_order_relaxed);
}

NativeRenderSplit TakeNativeRenderSplit() noexcept {
    NativeRenderSplit result;

    for (unsigned part = 0; part < NativeRenderSplit::Parts; ++part)
        result.samples[part] = renderPartSamples[part].exchange(0, std::memory_order_relaxed);

    result.packets = renderSplitPackets.exchange(0, std::memory_order_relaxed);
    return result;
}

void ReportNativeRenderThreadStall() {
    // Once per statistics interval: an unchanged stage, packet and step over
    // two reports means the render thread made no progress for over a second.
    static uint32_t lastStage = 0, lastStep = 0, reports = 0;
    static uint64_t lastPacket = 0;
    const auto stage = renderStage.load(std::memory_order_relaxed), step = renderStep.load(std::memory_order_relaxed);
    const auto packet = renderPacket.load(std::memory_order_relaxed);

    if (!running.load(std::memory_order_acquire) || stage == uint32_t(RenderStage::Waiting) || stage != lastStage || step != lastStep || packet != lastPacket) {
        lastStage = stage, lastStep = step, lastPacket = packet, reports = 0;
        return;
    }

    if (++reports < 2)
        return;

    constexpr const char* stages[]{"waiting", "tasks before draws", "frame begin", "draw", "frame end", "tasks after draws", "stop tasks"};

    if (stage == uint32_t(RenderStage::Draw))
        std::fprintf(stderr, "MH3 render thread: no progress for %u reports in %s of packet %llu, record %u kind %u (game sequence %llu)\n", reports, stages[stage],
                     static_cast<unsigned long long>(packet), step >> 8, step & 0xff, static_cast<unsigned long long>(sequence));
    else
        std::fprintf(stderr, "MH3 render thread: no progress for %u reports in %s of packet %llu, step %u (game sequence %llu)\n", reports, stages[stage < 7 ? stage : 0],
                     static_cast<unsigned long long>(packet), step, static_cast<unsigned long long>(sequence));
}

void TakeNativePacketSnapshotCounts(uint32_t out[3][6]) {
    static_assert(sizeof snapshotCounts == sizeof(uint32_t[3][6]));
    std::memcpy(out, snapshotCounts, sizeof snapshotCounts);
    std::memset(snapshotCounts, 0, sizeof snapshotCounts);
}

void TakeNativePacingWait(uint64_t& ns, uint32_t& waits) {
    ns = pacingWaitNs;
    waits = pacingWaits;
    pacingWaitNs = 0;
    pacingWaits = 0;
}

void TakeNativeGeometryBatchCounts(uint32_t& submitted, uint32_t& merged, uint32_t unmerged[6]) {
    submitted = geometrySubmitted;
    merged = geometryMerged;
    std::memcpy(unmerged, geometryUnmerged, 6 * sizeof(uint32_t));
    geometrySubmitted = geometryMerged = 0;
    std::memset(geometryUnmerged, 0, sizeof geometryUnmerged);
}
#endif
// The game thread limiter: the game thread records packet s only after the
// render thread consumed packet s - 2, that is submitted its frame to the GPU
// (completed), so it records s while the render thread works on s - 1 and the
// GPU on s - 2. The render thread waits for a free GPU frame slot instead of
// skipping a packet's draw (WaitForGpuFrameSlot), so every packet is drawn and
// the game runs at the rate frames are shown (variable steps follow it, so
// the frames sample game time evenly). A deeper
// queue only added a render frame of latency per packet. Without the limiter
// (the launcher's game thread limit off) the game thread waits only for a
// free slot, which the render thread frees by taking the newest packet,
// skipping the older ones: their draw work is lost and the frames shown
// judder. The wait spins briefly, then parks on
// takenSignal, which the render thread advances at each take and completion. The park is
// bounded and the loop re-checks, so a slot freed by a completion alone is
// seen within the bound, and a stall reports itself.
void PaceNativeFramePacket() {
    if (synchronous || !running.load(std::memory_order_relaxed))
        return;

    const uint64_t next = sequence + 1;
    const auto released = [next] { return paced ? completed.load(std::memory_order_acquire) + 2 >= next : SlotReleased(next); };
    const auto ready = [&] { return released() && SlotFree(next); };

    // Loaded before the check: a take after the check moves the signal on.
    uint32_t seen = takenSignal.Load();

    if (ready())
        return;
#ifdef MH3_DEVELOPMENT
    const uint64_t start = RenderMonotonicNs();
    uint64_t report = start + 2'000'000'000;
#endif
    while (!ready() && !failed.load(std::memory_order_acquire)) {
        // Released but still read by the GPU: no signal marks its fence, so
        // poll it.
        seen = takenSignal.AwaitChangeFor(seen, 50, released() ? 250 : 100000);
#ifdef MH3_DEVELOPMENT
        // The statistics come from this thread, so a stall reports itself.
        if (const uint64_t now = RenderMonotonicNs(); now >= report) {
            report = now + 2'000'000'000;
            std::fprintf(stderr, "MH3 pacing: game waited %.1f s to record packet %llu; render took %llu, completed %llu, stage %u step %u of packet %llu\n", (now - start) / 1e9,
                         static_cast<unsigned long long>(next), static_cast<unsigned long long>(taken.load()), static_cast<unsigned long long>(completed.load()),
                         renderStage.load(), renderStep.load(), static_cast<unsigned long long>(renderPacket.load()));
        }
#endif
    }
#ifdef MH3_DEVELOPMENT
    pacingWaitNs += RenderMonotonicNs() - start;
    ++pacingWaits;
#endif
}

void BeginNativeFramePacket(bool render) {
    RequireGame();
    RethrowFailure();

    // The render thread takes every sequence in order.
    if (recording)
        MH3_THROW(std::runtime_error("Frame packet begun before the last was published"));

    // PaceNativeFramePacket waited for it.
    if (!SlotFree(sequence + 1))
        MH3_THROW(std::runtime_error("Frame packet begun before its slot is free"));

    auto& p = packets[(sequence + 1) % kNativeFramePacketSlots];
    p.Reset(++sequence, render);
#ifdef MH3_DEVELOPMENT
    p.gameUs = uint64_t(GameStep::Elapsed() * (1e6 / 30.0));
    p.inputNs = RenderMonotonicNs();
#endif
    recording = &p;
}

bool NativeFramePacketRecording() {
    return recording && recording->render && !recording->canceled;
}

void CancelNativeFramePacket() {
    if (recording)
        recording->canceled = true;
}

// Statistics: the records already there when the game-side tail begins (those
// the frame's draw made before the scene's handoff).
uint32_t recordsBeforeDraw = 0;

uint32_t TakeNativeRecordsBeforeDraw() {
    return std::exchange(recordsBeforeDraw, 0);
}

void NativeFramePacketProducerChanged() {
    if (!recording)
        return;

    auto& p = *recording;
    p.draws.valid = p.materials.valid = p.lighting.valid = p.posts.valid = p.textures.valid = p.colors.valid = false;
}

void BeginNativeFramePacketTail() {
    if (!recording)
        return;

    recording->tailBegin = recording->records.size();
    recordsBeforeDraw += uint32_t(recording->records.size());
}

void EndNativeFramePacketTail() {
    if (recording && recording->tailBegin != SIZE_MAX)
        recording->tailEnd = recording->records.size();
}

#ifdef MH3_DEVELOPMENT
uint32_t NativeSkippedMeshes() {
    SkippedKinds();
    return skipMeshes;
}
#endif

void SetNativeFramePacketPrepareOnDraw(bool running) {
    prepareOnDraw.store(running, std::memory_order_release);
}

bool NativeFramePacketPrepareOnDraw() {
    return prepareOnDraw.load(std::memory_order_acquire);
}

void PublishNativeFramePacket(std::string capture) {
    RequireGame();

    if (!recording)
        MH3_THROW(std::runtime_error("Frame packet published without a begin"));

    auto& p = *recording;
    recording = nullptr;

    // The game-side tail, recorded before the scene, replays after it.
    if (p.tailBegin != SIZE_MAX)
        std::rotate(p.records.begin() + std::ptrdiff_t(p.tailBegin), p.records.begin() + std::ptrdiff_t(p.tailEnd), p.records.end());

    p.capture = std::move(capture);
    const bool wait = synchronous || !p.capture.empty();
#ifdef MH3_DEVELOPMENT
    p.publishedNs = RenderMonotonicNs();
#endif
    lastPublished.store(p.sequence, std::memory_order_release);
    published.Advance();

    if (wait)
        WaitCompleted(p.sequence);

    RethrowFailure();
}
