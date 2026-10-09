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
struct DisplayStatistics {
  uint32_t queued = 0, retired = 0, presented = 0, failures = 0;
  uint32_t backlog = 0, pending_scenes = 0;
  uint64_t callback_wait_us = 0;
};
DisplayStatistics display_statistics() noexcept;

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
  const void* display_pixels() const noexcept { return m_displayData[m_frames.slot()]; }
  static constexpr uint32_t Width = 960, Height = 544, Stride = 960;

private:
  void release() noexcept;
  bool m_initialized = false, m_inScene = false;
  // Slot 0 is owned by this application context for ordered scene retirement.
  volatile unsigned* m_notification = nullptr;
  uint32_t m_sceneSerial = 0;
  uint32_t m_sceneFlags = 0;
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
