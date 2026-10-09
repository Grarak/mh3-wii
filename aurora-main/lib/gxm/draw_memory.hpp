#pragma once
#include <cstddef>
#include <cstdint>
#include <array>
#include <psp2/gxm.h>
#include "memory.hpp"

namespace aurora::gxm {
class Device;

// The renderer owns one binding state. Draw payloads are borrowed from the
// active staging slot; only the immutable numeric-ID stream is allocated here.
class DrawMemory {
public:
  explicit DrawMemory(Device& device);
  ~DrawMemory();
  DrawMemory(const DrawMemory&) = delete;
  DrawMemory& operator=(const DrawMemory&) = delete;
  void bind(const void* uniforms, size_t uniformBytes, const void* indices, size_t indexBytes, const void* vertices, size_t vertexBytes, const void* attributes, size_t attributeBytes);
  SceGxmNotification completion_fence();
  void bind_uniform_range(uint32_t offset, uint32_t bytes);

  // Shaded draws: vertex BUFFER[0] draw, [1] scene, [2] draw + paletteOffset
  // (paletteBytes 0 = none); fragment BUFFER[0] draw + shadeOffset (shadeBytes
  // 0 = none: material-GPU programs), [1] scene.
  void bind_shaded(const void* draw, uint32_t drawBytes, uint32_t shadeOffset, uint32_t shadeBytes, uint32_t paletteOffset, uint32_t paletteBytes, const void* scene, uint32_t sceneBytes);
  void draw(uint32_t indexCount, uint32_t indexOffset, uint32_t instances);

private:
  Device& m_device;
  MappedMemory m_ids;
  const void* m_uniforms = nullptr;
  const void* m_indices = nullptr;
  size_t m_uniformBytes = 0, m_indexBytes = 0;
  SceGxmNotification m_lastUse{};
};

// Bind the producer's mapped memory directly. Its vertex/uniform/index spans
// share the owning slot until its last scene notification completes. The
// recorded-GX attribute-word span is empty for native frame slots.
SceGxmNotification draw_memory_fence();
bool gpu_frame_pending() noexcept;
std::array<uint64_t, 2> finish_wait_counters() noexcept;
void bind_draw_memory(const void* uniforms, size_t uniformBytes, const void* indices, size_t indexBytes, const void* vertices, size_t vertexBytes, const void* attributes, size_t attributeBytes);
} // namespace aurora::gxm
