// Game -> render thread frame packets (native_frame_packet.h).
#include "native_frame_packet.h"
#include "bytes_equal.h"
#include "native_graphics.h"
#include "native_shade_snapshot.h"
#include "frame_split.h"
#include "runtime/native/adapters/model_scope_bridge.h"
#include "runtime/native/mh3_frame_state_exchange.h"
#include "model_render_prep.h"
#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#include "port_check.h"
#endif
#include <aurora/aurora.h>
#include "gl/gl.hpp"
#include "gl/device.hpp"
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

namespace aurora::gfx {
uint32_t current_frame() noexcept;
}

namespace {
// Per-frame geometry high water is about 200 KiB of vertices (Alatreon quest).
constexpr size_t VertexArenaBytes = 2u << 20, IndexArenaBytes = 512u << 10;

enum class Kind : uint8_t { Model, MaterialGeometry, Geometry, DepthClear, Capture, Glare, Clone };
using StateIndices = std::array<uint32_t, NATIVE_STATE_COMPONENTS>;
struct Record {
    Kind kind;
    bool textured;
    uint32_t payload;
    StateIndices state;
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
#ifdef MH3_DEVELOPMENT
    uint64_t publishedNs = 0; // RenderMonotonicNs at publication
#endif
    void Reset(uint64_t next, bool admitted) {
        sequence = next;
        render = admitted;
        canceled = false;
        capture.clear();
        records.clear();
        materialGeometry.clear();
        geometry.clear();
        feedbackCount = 0;
        draws.Reset();
        materials.Reset();
        lighting.Reset();
        posts.Reset();
        textures.Reset();
        colors.Reset();
        vertexCursor = indexCursor = 0;
    }
};
struct Task {
    uint64_t sequence;
    std::function<void()> run;
};

mh3_frame_state_exchange exchange{};
mh3_frame_state_producer producer{};
mh3_frame_state_consumer consumer{};
std::array<Packet, 3> packets;
std::thread renderThread;
std::thread::id gameThread;
std::atomic<bool> running{false}, stopping{false}, failed{false};
std::exception_ptr failure;
// published: packets handed over (render thread waits on it); completed: the
// sequence of the last packet the render thread finished (game waits on it).
std::atomic<uint32_t> published{0};
std::atomic<uint64_t> completed{0};
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
#else
#define MH3_RENDER_STAGE(...)
#endif
bool synchronous = false;
std::mutex taskLock;
std::deque<Task> beforeTasks, afterTasks;

// Game thread.
Packet* recording = nullptr;
uint64_t sequence = 0;
struct FeedbackTarget {
    uint32_t width, height;
    uint64_t revision;
};
std::unordered_map<const void*, FeedbackTarget> feedbackTargets;
uint64_t feedbackRevision = 0;

void RequireGame() {
    if (std::this_thread::get_id() != gameThread && running)
        throw std::runtime_error("Frame packet recorded outside the game thread");
}
void RethrowFailure() {
    if (failed.load(std::memory_order_acquire))
        std::rethrow_exception(failure);
}
Packet& ProducerPacket() {
    void* data = nullptr;
    size_t bytes = 0;
    if (mh3_frame_state_producer_buffer(&producer, &data, &bytes) != MH3_FRAME_STATE_OK)
        throw std::runtime_error("Frame packet exchange has no producer buffer");
    return *static_cast<Packet*>(data);
}

#ifdef MH3_DEVELOPMENT
// Per component: draw records, captures (revision advanced) and stored copies.
uint32_t snapshotCounts[3][NATIVE_STATE_COMPONENTS];
// Material geometry submissions, and those appended to the previous draw.
uint32_t geometrySubmitted, geometryMerged;
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
    const uint32_t revision = nativeStateRevision[unsigned(NativeOwner::Game)][component];
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
// pre-transformed geometry), to attribute GPU time to them.
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
            else
                std::fprintf(stderr, "MH3: MH3_PROFILE: skip=%.*s is not model, material or geometry\n", int(kind.size()), kind.data());
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
    for (uint32_t index = 0; const auto& record : p.records) {
        MH3_RENDER_STAGE(RenderStage::Draw, index << 8 | uint32_t(record.kind));
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
                throw std::runtime_error("Scene feedback clone source was not captured");
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
        Restore(p, record.state, applied);
        switch (record.kind) {
        case Kind::Model:
            ExecuteNativeModelDraw(p.slot, record.payload);
            break;
        case Kind::MaterialGeometry: {
#ifdef __vita__
            AuroraNativeMaterialGeometry geometry;
            if (GpuGeometry(p.materialGeometry[record.payload], geometry, aurora_allocate_native_material_geometry))
                DrawNativeMaterialGeometry(geometry);
#else
            DrawNativeMaterialGeometry(p.materialGeometry[record.payload]);
#endif
            break;
        }
        case Kind::Geometry: {
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
    RunTasks(beforeTasks, p.sequence);
    if (p.render && !p.canceled) {
        MH3_RENDER_STAGE(RenderStage::Begin);
        if (aurora_begin_native_frame())
            Draw(p);
        if (!p.capture.empty())
            aurora_request_frame_capture(aurora::gfx::current_frame() + 1, p.capture.c_str());
        MH3_RENDER_STAGE(RenderStage::End);
        aurora_end_native_frame();
    }
    MH3_RENDER_STAGE(RenderStage::AfterTasks);
    RunTasks(afterTasks, p.sequence);
    MH3_RENDER_STAGE(RenderStage::Waiting);
}
// Microseconds on a cheap clock (the Vita's steady_clock is a system call).
uint64_t PacketWaitClockUs() noexcept {
#ifdef __vita__
    return sceKernelGetProcessTimeWide();
#else
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
// Waits until a packet after `ticket` is published (or the thread stops,
// which also publishes). Packets come in bursts: poll for a millisecond
// first, then block. On the Vita, std::atomic::wait sits on a pthread
// condition variable that can lose wakeups (there is no futex), so it sleeps
// in short steps there instead.
void WaitForPacket(uint32_t ticket) {
    constexpr uint64_t SpinUs = 1000;
    const uint64_t spinEnd = PacketWaitClockUs() + SpinUs;
    while (published.load(std::memory_order_acquire) == ticket) {
        if (PacketWaitClockUs() < spinEnd)
            continue;
#ifdef __vita__
        sceKernelDelayThread(100);
#else
        published.wait(ticket, std::memory_order_acquire);
#endif
    }
}
void RenderLoop() {
    aurora::gl::acquire_context();
    try {
#ifdef __vita__
        // The game thread runs on core 2; the AX mix worker and the display
        // callback on core 0.
        if (sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_1) < 0)
            throw std::runtime_error("Render thread core 1 affinity failed");
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
            const uint32_t ticket = published.load(std::memory_order_acquire);
            const auto status = mh3_frame_state_acquire(&consumer);
            if (status == MH3_FRAME_STATE_EMPTY) {
                if (stopping.load(std::memory_order_acquire))
                    break;
#ifdef MH3_DEVELOPMENT
                if (!idleStart)
                    idleStart = RenderMonotonicNs();
#endif
                WaitForPacket(ticket);
                continue;
            }
            if (status != MH3_FRAME_STATE_OK)
                throw std::runtime_error("Frame packet exchange acquire failed");
            void* data = nullptr;
            size_t bytes = 0;
            uint64_t generation = 0;
            if (mh3_frame_state_consumer_buffer(&consumer, &data, &bytes, &generation) != MH3_FRAME_STATE_OK)
                throw std::runtime_error("Frame packet exchange has no consumer buffer");
            const auto& p = *static_cast<const Packet*>(data);
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
            Execute(p);
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
        }
        // Resource work queued after the last packet.
        MH3_RENDER_STAGE(RenderStage::StopTasks);
        RunTasks(beforeTasks, UINT64_MAX);
        RunTasks(afterTasks, UINT64_MAX);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "MH3 render thread: %s\n", error.what());
        failure = std::current_exception();
        failed.store(true, std::memory_order_release);
    } catch (...) {
        failure = std::current_exception();
        failed.store(true, std::memory_order_release);
    }
    completed.store(UINT64_MAX, std::memory_order_release);
    completed.notify_all();
#ifdef MH3_DEVELOPMENT
    renderCounters.thread.store(-1, std::memory_order_release);
#endif
    aurora::gl::release_context();
}
void WaitCompleted(uint64_t target) {
    for (uint64_t seen = completed.load(std::memory_order_acquire); seen < target; seen = completed.load(std::memory_order_acquire))
        completed.wait(seen, std::memory_order_acquire);
    RethrowFailure();
}
} // namespace

bool OnNativeGraphicsThread() {
    return aurora::gl::on_render_thread() || (running.load(std::memory_order_relaxed) && std::this_thread::get_id() == gameThread);
}

bool AllocateNativeGeometry(uint32_t vertices, uint32_t indices, AuroraNativeGeometry& out) {
    return Allocate(vertices, indices, out);
}
bool AllocateNativeMaterialGeometry(uint32_t vertices, uint32_t indices, AuroraNativeMaterialGeometry& out) {
    return Allocate(vertices, indices, out);
}
void RecordNativeMaterialGeometry(const AuroraNativeMaterialGeometry& geometry) {
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
        throw std::runtime_error("Scene feedback has no producer identity");
    auto& op = PushFeedback(Kind::Capture);
    op.identity = identity;
    op.capture = parameters;
    feedbackTargets[identity] = {parameters.width, parameters.height, ++feedbackRevision};
}
void RecordNativeSceneGlare(const void* identity, const aurora::SceneGlareParameters& parameters) {
    if (!NativeFramePacketRecording())
        return;
    if (!identity)
        throw std::runtime_error("Scene feedback has no producer identity");
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
        throw std::runtime_error("No frame packet is being recorded");
    return recording->slot;
}
uint64_t NativeFramePacketSequence() {
    return sequence;
}
uint64_t NativeFramePacketsDone() {
    // The render thread takes packets in sequence order: once it finished
    // one, every older packet has rendered or was replaced and is never read.
    return running.load(std::memory_order_acquire) ? completed.load(std::memory_order_acquire) : UINT64_MAX;
}

void StartNativeRenderThread() {
    if (running)
        throw std::runtime_error("Render thread already started");
    if (!aurora::gl::on_render_thread())
        throw std::runtime_error("Render thread started outside the graphics owner");
    if (mh3_frame_state_init(&exchange, &producer, &consumer, &packets[0], &packets[1], &packets[2], sizeof(Packet)) != MH3_FRAME_STATE_OK)
        throw std::runtime_error("Frame packet exchange initialization failed");
    for (uint32_t i = 0; i < packets.size(); ++i)
        packets[i].slot = i;
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
#endif
    gameThread = std::this_thread::get_id();
    stopping = false;
    failed = false;
    // A restart (the launcher's renderer, then the game's) continues the
    // packet sequence; the previous thread left completed at its maximum.
    completed.store(sequence, std::memory_order_relaxed);
    aurora::gl::release_context();
    running = true;
    renderThread = std::thread(RenderLoop);
}
void StopNativeRenderThread() {
    if (!running)
        return;
    stopping.store(true, std::memory_order_release);
    published.fetch_add(1, std::memory_order_release);
    published.notify_all();
    renderThread.join();
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
void TakeNativeGeometryBatchCounts(uint32_t& submitted, uint32_t& merged) {
    submitted = geometrySubmitted;
    merged = geometryMerged;
    geometrySubmitted = geometryMerged = 0;
}
#endif
void BeginNativeFramePacket(bool render) {
    RequireGame();
    RethrowFailure();
    auto& p = ProducerPacket();
    p.Reset(++sequence, render);
    recording = &p;
}
bool NativeFramePacketRecording() {
    return recording && recording->render && !recording->canceled;
}
void CancelNativeFramePacket() {
    if (recording)
        recording->canceled = true;
}
void PublishNativeFramePacket(std::string capture) {
    RequireGame();
    if (!recording)
        throw std::runtime_error("Frame packet published without a begin");
    auto& p = *recording;
    recording = nullptr;
    p.capture = std::move(capture);
    const bool wait = synchronous || !p.capture.empty();
    uint32_t replaced = 0;
#ifdef MH3_DEVELOPMENT
    p.publishedNs = RenderMonotonicNs();
#endif
    if (mh3_frame_state_publish(&producer, p.sequence, &replaced) != MH3_FRAME_STATE_OK)
        throw std::runtime_error("Frame packet exchange publish failed");
    published.fetch_add(1, std::memory_order_release);
    published.notify_one();
    if (wait)
        WaitCompleted(p.sequence);
    RethrowFailure();
}
