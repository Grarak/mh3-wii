#pragma once

#include "memory.hpp"
#include <aurora/gpu_completion.hpp>
#include <psp2/gxm.h>
#include <map>
#include <memory>
#include <utility>
#include <vector>
#include <span>

namespace aurora::gxm {

void check(int result, const char* operation);

// Game-thread toggle. Common dialogs render into the next admitted display
// surface after its application scene ends, using the same display sync.
void set_common_dialog_active(bool active) noexcept;
const SceGxmProgram* vertex_binary(uint64_t hash);
const SceGxmProgram* fragment_binary(uint64_t hash);
const SceGxmProgram* fragment_scalar_binary(uint64_t hash);

// Read on the graphics owner. Callback retirement releases display-queue
// bookkeeping; presented counts successful VBlank flips (SetFrameBuf + Wait).
// Pacing, for flips whose frame carries a game time (set_present_timing):
// paced_flips counts them, pacing_error_us adds |flip interval - game time the
// frame advanced| over them, pacing_error_max_us is the largest since the
// previous call, and flip_vblanks counts flip intervals of 1, 2, 3 and 4 or
// more VBlanks. Latency, for flips whose frame carries its input time:
// latency_us adds, per flip, the time from reading the input to the packet's
// publication (the game thread), to the present (the render thread's queue
// and replay), to the display callback (the GPU, and earlier flips still
// queued) and to the flip (VBlank), and latency_max_us is the largest
// input-to-flip time since the last call.
struct DisplayStatistics {
  uint32_t queued = 0, retired = 0, presented = 0, failures = 0;
  uint32_t backlog = 0, pending_scenes = 0;
  uint64_t callback_wait_us = 0;
  uint64_t paced_flips = 0, pacing_error_us = 0;
  uint32_t pacing_error_max_us = 0;
  uint64_t flip_vblanks[4]{};
  uint64_t latency_flips = 0, latency_us[4]{};
  uint32_t latency_max_us = 0;
};

DisplayStatistics display_statistics() noexcept;

// Flip trace (off by default): each paced flip's time and its frame's game
// time, both in microseconds, kept in a ring of the last 256 flips.
// take_flips copies those since the previous call (at most `capacity`, the
// newest) and returns how many; one reader.
struct FlipSample {
  uint64_t flip_us = 0, game_us = 0;
};

void set_flip_trace(bool enabled) noexcept;
size_t take_flips(FlipSample* out, size_t capacity) noexcept;

// With VSync: the VBlanks between flips beyond one each (a frame the display
// kept for more VBlanks because the next was late), summed since the start.
// Any thread.
int64_t repeated_vblanks() noexcept;

// VSync on (the default): flips at VBlank and waits; off: flips at VBlank
// without waiting (the display queue no longer paces the render thread),
// for the game thread limit off.
void set_vsync(bool enabled) noexcept;

// Owns native libgxm objects and bounded backing allocations. All calls belong
// to the render owner. The display callback is pinned to worker core 0.
// How a scene uses its depth surface's memory. Without a load each tile starts
// from `background` (a scene that begins with a depth clear, or depth no draw
// reads); without a store the scene's depth is discarded.
struct DepthScene {
  bool load = true, store = true;
  float background = 0.f;
};

class Device {
public:
  Device();
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  SceGxmContext* context() const noexcept { return m_context; }
  SceGxmShaderPatcher* patcher() const noexcept { return m_patcher; }
  CdramArena& arena() noexcept { return *m_arena; }
  bool scene_active() const noexcept { return m_inScene; }
  using DepthScene = gxm::DepthScene;
  void begin(uint32_t width, uint32_t height, const SceGxmColorSurface* color, const SceGxmDepthStencilSurface* depth, SceGxmSyncObject* sync = nullptr,
             DepthScene depthScene = {});
  void end();
  void finish();
  SceGxmNotification current_fence() const noexcept { return {m_notification, m_sceneSerial + unsigned(m_inScene)}; }
  SceGxmNotification completion_fence() {
    end();
    return current_fence();
  }

  bool frame_pending() const noexcept;
  uint32_t pending_scenes() const noexcept { return m_notification ? m_sceneSerial - *m_notification : 0; }
  unsigned frame_slot() const noexcept { return m_frames.slot(); }
  void begin_display();
  void flip();

  // Game time, input and publication time (microseconds, 0 = none) of the
  // frame the next flip() shows.
  void set_present_timing(uint64_t gameUs, uint64_t inputUs, uint64_t publishedUs) noexcept {
    m_presentGameUs = gameUs;
    m_presentInputUs = inputUs;
    m_presentPublishedUs = publishedUs;
  }

  const void* display_pixels() const noexcept { return m_displayData[m_frames.slot()]; }
  static constexpr uint32_t Width = 960, Height = 544, Stride = 960;

private:
  void release() noexcept;
  bool m_initialized = false, m_inScene = false;

  // Slot 0 is owned by this application context for ordered scene retirement.
  volatile unsigned* m_notification = nullptr;
  uint32_t m_sceneSerial = 0;
  aurora::GpuFrameRing m_frames;
  SceGxmContext* m_context = nullptr;
  SceGxmShaderPatcher* m_patcher = nullptr;
  void* m_host = nullptr;
  std::vector<MappedMemory> m_memory;
  std::unique_ptr<CdramArena> m_arena;
  std::map<std::pair<uint32_t, uint32_t>, SceGxmRenderTarget*> m_targets;
  void* m_displayData[aurora::GpuFrameRing::BufferCount]{};
  SceGxmColorSurface m_display[aurora::GpuFrameRing::BufferCount]{};
  SceGxmSyncObject* m_sync[aurora::GpuFrameRing::BufferCount]{};
  unsigned m_front = aurora::GpuFrameRing::BufferCount - 1;
  uint64_t m_presentGameUs = 0, m_presentInputUs = 0, m_presentPublishedUs = 0;
};

// Reflection supplies the vertex-ID / instance-ID stream register indices.
// Fragment variants include native blend equations and channel write masks.
struct VertexInput {
  const char* name;
  SceGxmAttributeFormat format;
  uint16_t components, offset, stride, stream;
};

class PatchedProgram {
public:
  PatchedProgram(Device& device, uint64_t vertexHash, uint64_t fragmentHash, SceGxmIndexFormat indices = SCE_GXM_INDEX_FORMAT_U16, std::span<const VertexInput> inputs = {});
  ~PatchedProgram();
  PatchedProgram(const PatchedProgram&) = delete;
  PatchedProgram& operator=(const PatchedProgram&) = delete;
  void bind(const SceGxmBlendInfo& blend, SceGxmOutputRegisterFormat output = SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4);
  void set_vertex_uniform(const char* name, const float* values, unsigned count);
  void set_fragment_uniform(const char* name, const float* values, unsigned count);
  bool uses_discard() const noexcept { return sceGxmProgramIsDiscardUsed(m_boundFragmentBinary ? m_boundFragmentBinary : m_fragmentBinary); }
  bool replaces_depth() const noexcept { return sceGxmProgramIsDepthReplaceUsed(m_boundFragmentBinary ? m_boundFragmentBinary : m_fragmentBinary); }
  // Vertex stream that supplies attributes the inputs left unbound; it reads
  // one 16-byte F32 (0, 0, 0, 1) element. Negative when every attribute is bound.
  int default_stream() const noexcept { return m_defaultStream; }

  // The patched programs and binaries bind() last set: precomputed states are
  // initialized for them.
  const SceGxmVertexProgram* vertex_program() const noexcept { return m_vertex; }
  const SceGxmFragmentProgram* fragment_program() const noexcept { return m_lastFragment; }
  const SceGxmProgram* vertex_gxp() const noexcept { return m_vertexBinary; }
  const SceGxmProgram* fragment_gxp() const noexcept { return m_boundFragmentBinary; }

private:
  Device& m_device;
  const SceGxmProgram* m_vertexBinary;
  const SceGxmProgram* m_fragmentBinary;
  const SceGxmProgram* m_boundFragmentBinary = nullptr;
  const SceGxmProgram* m_scalarFragmentBinary = nullptr;
  uint64_t m_fragmentHash;
  SceGxmShaderPatcherId m_vertexId = nullptr, m_fragmentId = nullptr;
  SceGxmShaderPatcherId m_scalarFragmentId = nullptr;
  SceGxmVertexProgram* m_vertex = nullptr;
  std::map<uint64_t, SceGxmFragmentProgram*> m_fragments;
  uint64_t m_lastKey = 0;
  SceGxmFragmentProgram* m_lastFragment = nullptr;
  void* m_vertexDefault = nullptr;
  void* m_fragmentDefault = nullptr;
  int m_defaultStream = -1;
  void release() noexcept;
};
} // namespace aurora::gxm
