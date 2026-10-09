#pragma once

#include "memory.hpp"
#include <psp2/gxm.h>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace aurora::gxm {

void check(int result, const char* operation);
const SceGxmProgram* vertex_binary(uint64_t hash);
const SceGxmProgram* fragment_binary(uint64_t hash);

// Owns native libgxm objects and bounded backing allocations. All calls belong
// to the render thread. The display callback is pinned to worker core 0.
class Device {
public:
  explicit Device(bool vsync);
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  SceGxmContext* context() const noexcept { return m_context; }
  SceGxmShaderPatcher* patcher() const noexcept { return m_patcher; }
  CdramArena& arena() noexcept { return *m_arena; }
  void begin(uint32_t width, uint32_t height, const SceGxmColorSurface* color,
             const SceGxmDepthStencilSurface* depth, SceGxmSyncObject* sync = nullptr);
  void end();
  void finish();
  void begin_display();
  void flip();
  const void* display_pixels() const noexcept { return m_displayData[m_back]; }
  static constexpr uint32_t Width = 960, Height = 544, Stride = 960;

private:
  void release() noexcept;
  bool m_initialized = false, m_inScene = false, m_vsync;
  SceGxmContext* m_context = nullptr;
  SceGxmShaderPatcher* m_patcher = nullptr;
  void* m_host = nullptr;
  std::vector<MappedMemory> m_memory;
  std::unique_ptr<CdramArena> m_arena;
  std::map<std::pair<uint32_t, uint32_t>, SceGxmRenderTarget*> m_targets;
  void* m_displayData[3]{};
  SceGxmColorSurface m_display[3]{};
  SceGxmSyncObject* m_sync[3]{};
  unsigned m_back = 0, m_front = 2;
};

// Reflection supplies the vertex-ID / instance-ID stream register indices.
// Fragment variants include native blend equations and channel write masks.
class PatchedProgram {
public:
  PatchedProgram(Device& device, uint64_t vertexHash, uint64_t fragmentHash);
  ~PatchedProgram();
  PatchedProgram(const PatchedProgram&) = delete;
  PatchedProgram& operator=(const PatchedProgram&) = delete;
  void bind(const SceGxmBlendInfo& blend);
  void set_vertex_uniform(const char* name, const float* values, unsigned count);
  void set_fragment_uniform(const char* name, const float* values, unsigned count);

private:
  Device& m_device;
  const SceGxmProgram* m_vertexBinary;
  const SceGxmProgram* m_fragmentBinary;
  SceGxmShaderPatcherId m_vertexId = nullptr, m_fragmentId = nullptr;
  SceGxmVertexProgram* m_vertex = nullptr;
  std::map<uint32_t, SceGxmFragmentProgram*> m_fragments;
  void* m_vertexDefault = nullptr;
  void* m_fragmentDefault = nullptr;
  void release() noexcept;
};
}
