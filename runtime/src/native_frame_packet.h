// Game -> render thread frame packets. The game thread records each frame as
// typed draw records (model items, material/UI geometry, depth clears, scene
// feedback) with the shading state captured per changed component; the render
// thread alone owns the graphics API: it restores each record's state, draws,
// and presents. Packets travel through the three-slot newest-wins exchange
// (mh3_frame_state_exchange): an unrendered packet may be replaced by a newer
// one, so resource work goes through the task queues instead.
#pragma once
#include <aurora/native_geometry.h>
#include <aurora/native_scene_feedback.hpp>
#include <cstdint>
#include <functional>
#include <string>

// ---- Game thread: recording into the current packet ----
// Geometry lives in the packet's arenas until the render thread drew it.
// False while not recording or when the arenas are full (frame canceled).
bool AllocateNativeGeometry(uint32_t vertices, uint32_t indices, AuroraNativeGeometry& out);
bool AllocateNativeMaterialGeometry(uint32_t vertices, uint32_t indices, AuroraNativeMaterialGeometry& out);
void RecordNativeMaterialGeometry(const AuroraNativeMaterialGeometry& geometry);
// UI geometry with the recorded viewport, scissor, raster and texture unit 0:
// textured draws sampled RGBA * vertex RGBA, otherwise vertex RGBA.
void SubmitNativeGeometry(const AuroraNativeGeometry& geometry, bool textured);
// item indexes the model store of this packet's slot (model_scope.cpp).
// A deferred item is prepared by the render thread into its own state.
void RecordNativeModelDraw(uint32_t item, bool deferred = false);
void RecordNativeDepthClear();
// Scene feedback runs in draw order on the render thread. The game thread
// keeps its own revisions of the published targets for its texture bindings.
void RecordNativeSceneCapture(const void* identity, const aurora::SceneCaptureParameters& parameters);
void RecordNativeSceneGlare(const void* identity, const aurora::SceneGlareParameters& parameters);
bool RecordNativeSceneClone(const void* destination, const void* source);
uint64_t NativeSceneFeedbackVersion(const void* identity, uint32_t width, uint32_t height);
// Resource work outside the draw stream. Publications run before the draws
// of the frame that queued them, retirements after them; a replaced packet
// never drops either. Runs inline while no render thread exists.
enum class NativeTaskOrder { BeforeDraws, AfterDraws };
void QueueNativeGraphicsTask(NativeTaskOrder order, std::function<void()> task);
// The recording packet's exchange slot (0..2) and frame sequence.
uint32_t NativeFramePacketSlot();
uint64_t NativeFramePacketSequence();
// Game thread: every packet up to this sequence has rendered or was replaced
// unrendered, so nothing it recorded is read any more (all of them while no
// render thread runs).
uint64_t NativeFramePacketsDone();

// ---- Frame lifecycle (graphics_host.cpp) ----
// Start hands the calling (game) thread's graphics context to a new render
// thread; stop drains it, runs every queued task and hands the context back.
// The launcher stops its render thread and the game's startup starts another.
void StartNativeRenderThread();
void StopNativeRenderThread();
bool NativeRenderThreadRunning();
// Begins the next packet; it records draws only when render is true.
void BeginNativeFramePacket(bool render);
bool NativeFramePacketRecording();
void CancelNativeFramePacket();
// capture: development frame dump path for this packet, or empty. A capture
// packet is waited for, so it cannot be replaced before it renders.
void PublishNativeFramePacket(std::string capture);

#ifdef MH3_DEVELOPMENT
#include "native_frame_statistics.h"
// Cumulative completed replay work, including ordered resource tasks and
// presentation, excluding the wait for another packet. packet_wait_ns is that
// wait: time the render thread sat with no packet, counted when the next
// packet arrives; packet_waits is how many packets it had to wait for. Draw packets are the
// admitted, uncanceled packets; their completion is not a display flip.
struct NativeRenderThreadStatistics {
    uint64_t packets = 0, draw_packets = 0;
    uint64_t packet_wall_ns = 0, draw_packet_wall_ns = 0;
    uint64_t packet_wait_ns = 0, packet_waits = 0;
    // Of the waits: publication-to-replay time (wake latency), how many
    // packets it was measured for, and how many took over 1 ms.
    uint64_t wake_ns = 0, wakes = 0, late_wakes = 0;
    uint64_t sampled_at_ns = 0;
    mh3::statistics::OwnerCpuSample cpu{};
    bool counters_available = false;
};
// Game thread, initial baseline and once per enabled statistics interval only.
// Samples the actual render TID, scheduled CPU clock, current CPU and affinity;
// bounded atomic reads add no lock, queue task, packet wait or GPU wait.
NativeRenderThreadStatistics ReadNativeRenderThreadStatistics() noexcept;
// Game thread: per state component (draw, material, lighting, post, textures,
// colours) the records, captures (revision advanced) and stored copies since
// the last call; resets them.
void TakeNativePacketSnapshotCounts(uint32_t out[3][6]);
// Game thread: material geometry submissions and those joined to the draw
// before them (same state, adjacent in draw order) since the last call.
void TakeNativeGeometryBatchCounts(uint32_t& submitted, uint32_t& merged);
// Game thread, once per statistics interval: names the render thread's stage,
// packet and record when it made no progress since the last two calls.
void ReportNativeRenderThreadStall();
#endif
