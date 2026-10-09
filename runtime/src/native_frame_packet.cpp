// Game -> render thread frame packets (native_frame_packet.h).
#include "native_frame_packet.h"
#include "native_graphics.h"
#include "native_shade_snapshot.h"
#include "frame_split.h"
#include "draw_replica.h"
#include "runtime/native/adapters/model_instance_bridge.h"
#include "runtime/native/adapters/model_scope_bridge.h"
#include "runtime/native/mh3_frame_state_exchange.h"
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
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef MH3_DEVELOPMENT
#include <chrono>
#endif
#ifdef __vita__
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

enum class Kind : uint8_t { Model, Instance, MaterialGeometry, Geometry, DepthClear, Capture, Glare, Clone };
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
    bool valid = false;
    void Reset() { count = 0, valid = false; }
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
    std::unique_ptr<uint8_t[]> vertices{new uint8_t[VertexArenaBytes]}, indices{new uint8_t[IndexArenaBytes]};
    size_t vertexCursor = 0, indexCursor = 0;
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
bool synchronous = false;
std::mutex taskLock;
std::deque<Task> beforeTasks, afterTasks;
// The packet that mirrored host-state mutations belong to (any thread).
std::atomic<uint64_t> replicaSequence{1};

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

template <class T, void (*Capture)(T&)>
uint32_t Component(Pool<T>& pool, NativeStateComponent component) {
    const uint32_t revision = nativeStateRevision[component];
    if (pool.valid && pool.revision == revision)
        return pool.current;
    if (pool.count == pool.items.size())
        pool.items.emplace_back();
    auto& next = pool.items[pool.count];
    Capture(next);
    pool.revision = revision;
    // Producers republish unchanged state; keep one copy of equal contents.
    if (pool.valid && !std::memcmp(&next, &pool.items[pool.current], sizeof next))
        return pool.current;
    pool.valid = true;
    pool.current = pool.count++;
    return pool.current;
}
void Push(Kind kind, uint32_t payload, bool textured = false) {
    auto& p = *recording;
    Record record{kind, textured, payload, {}};
    record.state[NATIVE_STATE_DRAW] = Component<NativeDrawSnapshot, CaptureNativeDrawSnapshot>(p.draws, NATIVE_STATE_DRAW);
    record.state[NATIVE_STATE_MATERIAL] = Component<NativeMaterialSnapshot, CaptureNativeMaterialSnapshot>(p.materials, NATIVE_STATE_MATERIAL);
    record.state[NATIVE_STATE_LIGHTING] = Component<NativeLightingSnapshot, CaptureNativeLightingSnapshot>(p.lighting, NATIVE_STATE_LIGHTING);
    record.state[NATIVE_STATE_POST] = Component<NativePostSnapshot, CaptureNativePostSnapshot>(p.posts, NATIVE_STATE_POST);
    record.state[NATIVE_STATE_TEXTURES] = Component<NativeTextureSnapshot, CaptureNativeTextureSnapshot>(p.textures, NATIVE_STATE_TEXTURES);
    p.records.push_back(record);
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
    for (;;) {
        std::function<void()> run;
        {
            std::lock_guard lock(taskLock);
            if (queue.empty() || queue.front().sequence > through)
                return;
            run = std::move(queue.front().run);
            queue.pop_front();
        }
        run();
    }
}
void Restore(const Packet& p, const StateIndices& state, StateIndices& applied) {
    // Draw state first: the material snapshot already holds its raster import.
    if (applied[NATIVE_STATE_DRAW] != state[NATIVE_STATE_DRAW])
        RestoreNativeDrawSnapshot(p.draws.items[state[NATIVE_STATE_DRAW]]);
    if (applied[NATIVE_STATE_LIGHTING] != state[NATIVE_STATE_LIGHTING])
        RestoreNativeLightingSnapshot(p.lighting.items[state[NATIVE_STATE_LIGHTING]]);
    if (applied[NATIVE_STATE_POST] != state[NATIVE_STATE_POST])
        RestoreNativePostSnapshot(p.posts.items[state[NATIVE_STATE_POST]]);
    if (applied[NATIVE_STATE_TEXTURES] != state[NATIVE_STATE_TEXTURES])
        RestoreNativeTextureSnapshot(p.textures.items[state[NATIVE_STATE_TEXTURES]]);
    if (applied[NATIVE_STATE_MATERIAL] != state[NATIVE_STATE_MATERIAL])
        RestoreNativeMaterialSnapshot(p.materials.items[state[NATIVE_STATE_MATERIAL]]);
    applied = state;
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
void Draw(const Packet& p) {
    const auto& color = aurora::gpu::g_frameBuffer;
    const auto& depth = aurora::gpu::g_depthBuffer;
    aurora::gl::bind_framebuffer(color.texture->gl(), depth.texture->gl());
    aurora::gl::set_viewport(0, 0, color.width, color.height, 0.f, 1.f);
    aurora::gl::set_scissor_enabled(false);
    aurora::gl::set_color_mask(true, true, true, true);
    aurora::gl::set_depth_write(true);
    const float black[4]{0.f, 0.f, 0.f, 1.f};
    aurora::gl::clear_color(black);
    aurora::gl::clear_depth(0.f); // native shader projection uses reversed Z
    StateIndices applied;
    const auto forget = [&] {
        applied.fill(UINT32_MAX);
        ForgetNativeTextureBindings();
    };
    forget();
    for (const auto& record : p.records) {
        if (!aurora_native_frame_should_render())
            return;
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
        Restore(p, record.state, applied);
        switch (record.kind) {
        case Kind::Model:
            ExecuteNativeModelDraw(p.slot, record.payload);
            break;
        case Kind::Instance:
            ExecuteNativeModelInstance(p.slot, record.payload);
            // The instance changed the drawing state its record restored.
            forget();
            break;
        case Kind::MaterialGeometry: {
#ifdef __vita__
            AuroraNativeMaterialGeometry geometry;
            if (GpuGeometry(p.materialGeometry[record.payload], geometry, aurora_allocate_native_material_geometry))
                SubmitNativeMaterialGeometry(geometry);
#else
            SubmitNativeMaterialGeometry(p.materialGeometry[record.payload]);
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
    RunTasks(beforeTasks, p.sequence);
    if (p.render && !p.canceled) {
        if (aurora_begin_native_frame())
            Draw(p);
        if (!p.capture.empty())
            aurora_request_frame_capture(aurora::gfx::current_frame() + 1, p.capture.c_str());
        aurora_end_native_frame();
    }
    RunTasks(afterTasks, p.sequence);
}
void RenderLoop() {
    aurora::gl::acquire_context();
    DrawReplica::SetCurrent(DrawReplica::Render);
#ifdef MH3_DEVELOPMENT
    mh3SplitOff = true;
#endif
    try {
#ifdef __vita__
        // The game thread runs on core 2; the AX mix worker and the display
        // callback on core 0.
        if (sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_1) < 0)
            throw std::runtime_error("Render thread core 1 affinity failed");
#endif
#ifdef MH3_DEVELOPMENT
        uint64_t replayed = 0, drawPackets = 0, wallNs = 0, drawWallNs = 0;
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
                published.wait(ticket, std::memory_order_acquire);
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
            renderCounters.revision.fetch_add(1, std::memory_order_release);
#endif
            completed.store(p.sequence, std::memory_order_release);
            completed.notify_all();
        }
        // Resource work queued after the last packet.
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
    recording->materialGeometry.push_back(geometry);
    Push(Kind::MaterialGeometry, uint32_t(recording->materialGeometry.size() - 1));
}
void SubmitNativeGeometry(const AuroraNativeGeometry& geometry, bool textured) {
    if (!NativeFramePacketRecording() || !geometry.index_count)
        return;
    recording->geometry.push_back(geometry);
    Push(Kind::Geometry, uint32_t(recording->geometry.size() - 1), textured);
}
void RecordNativeModelDraw(uint32_t item) {
    if (NativeFramePacketRecording())
        Push(Kind::Model, item);
}
void RecordNativeModelInstance(uint32_t index) {
    if (NativeFramePacketRecording())
        Push(Kind::Instance, index);
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
void QueueNativeReplicaTask(std::function<void()> task, bool after) {
    std::lock_guard lock(taskLock);
    (after ? afterTasks : beforeTasks).push_back({replicaSequence.load(std::memory_order_acquire), std::move(task)});
}
uint32_t NativeFramePacketSlot() {
    if (!recording)
        throw std::runtime_error("No frame packet is being recorded");
    return recording->slot;
}
uint64_t NativeFramePacketSequence() {
    return sequence;
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
    // MH3_RENDER_SYNC=1: wait for every packet to render (serial frames).
    if (const char* v = std::getenv("MH3_RENDER_SYNC"))
        synchronous = v[0] == '1';
#endif
    gameThread = std::this_thread::get_id();
    stopping = false;
    failed = false;
    replicaSequence.store(sequence + 1, std::memory_order_release);
    StartNativeModelInstances();
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
    StopNativeModelInstances();
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
        std::atomic_thread_fence(std::memory_order_acquire);
        if (before == renderCounters.revision.load(std::memory_order_relaxed)) {
            result.counters_available = thread >= 0;
            break;
        }
    }
    return result;
}
#endif
void BeginNativeFramePacket(bool render) {
    RequireGame();
    RethrowFailure();
    auto& p = ProducerPacket();
    p.Reset(++sequence, render);
    recording = &p;
    replicaSequence.store(sequence, std::memory_order_release);
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
    replicaSequence.store(sequence + 1, std::memory_order_release);
    p.capture = std::move(capture);
    const bool wait = synchronous || !p.capture.empty();
    uint32_t replaced = 0;
    if (mh3_frame_state_publish(&producer, p.sequence, &replaced) != MH3_FRAME_STATE_OK)
        throw std::runtime_error("Frame packet exchange publish failed");
    published.fetch_add(1, std::memory_order_release);
    published.notify_one();
    if (wait)
        WaitCompleted(p.sequence);
    RethrowFailure();
}
