#include "draw_memory.hpp"
#include "device.hpp"
#include <limits>
#include <stdexcept>

namespace aurora::gxm {
namespace {
DrawMemory* g_draw = nullptr;
constexpr size_t UniformAlignment = 16, IndexAlignment = sizeof(uint16_t);
void require_alignment(const void* pointer, size_t bytes, size_t alignment) {
  if (bytes && (!pointer || (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1))))
    throw std::runtime_error("Unaligned native GXM draw buffer");
}
} // namespace
DrawMemory::DrawMemory(Device& device) : m_device(device), m_ids(65536 * sizeof(float), MappedMemory::Kind::Draw, "Aurora vertex and instance IDs") {
  if (g_draw)
    throw std::runtime_error("Native draw memory already initialized");
  auto* ids = static_cast<float*>(m_ids.data());
  for (unsigned i = 0; i < 65536; ++i)
    ids[i] = static_cast<float>(i);
  g_draw = this;
}
DrawMemory::~DrawMemory() {
  m_device.finish();
  g_draw = nullptr;
}
void DrawMemory::bind(const void* uniforms, size_t uniformBytes, const void* indices, size_t indexBytes, const void* vertices, size_t vertexBytes, const void* attributes, size_t attributeBytes) {
  require_alignment(uniforms, uniformBytes, UniformAlignment);
  require_alignment(indices, indexBytes, IndexAlignment);
  require_alignment(vertices, vertexBytes, UniformAlignment);
  require_alignment(attributes, attributeBytes, UniformAlignment);
  m_lastUse = {};
  m_uniforms = uniforms;
  m_uniformBytes = uniformBytes;
  m_indices = indices;
  m_indexBytes = indexBytes;
}
void DrawMemory::bind_shaded(const void* draw, uint32_t drawBytes, uint32_t shadeOffset, uint32_t paletteOffset, uint32_t paletteBytes, const void* scene, uint32_t sceneBytes) {
  const auto inside = [&](const void* pointer, uint32_t bytes) {
    const auto at = reinterpret_cast<uintptr_t>(pointer), base = reinterpret_cast<uintptr_t>(m_uniforms);
    return m_uniforms && at >= base && !((at - base) & (UniformAlignment - 1)) && at - base <= m_uniformBytes && bytes <= m_uniformBytes - (at - base);
  };
  if (!inside(draw, drawBytes) || shadeOffset >= drawBytes || (shadeOffset & 15) || !inside(scene, sceneBytes) ||
      (paletteBytes && ((paletteOffset & 15) || uint64_t(paletteOffset) + paletteBytes > drawBytes)))
    throw std::runtime_error("Shaded uniform blocks exceed the active draw slot");
  m_lastUse = m_device.current_fence();
  auto* context = m_device.context();
  check(sceGxmSetVertexUniformBuffer(context, 0, draw), "bind shaded draw block");
  check(sceGxmSetVertexUniformBuffer(context, 1, scene), "bind shaded vertex scene block");
  if (paletteBytes)
    check(sceGxmSetVertexUniformBuffer(context, 2, static_cast<const char*>(draw) + paletteOffset), "bind shaded palette block");
  check(sceGxmSetFragmentUniformBuffer(context, 0, static_cast<const char*>(draw) + shadeOffset), "bind shaded fragment block");
  check(sceGxmSetFragmentUniformBuffer(context, 1, scene), "bind shaded fragment scene block");
}
void DrawMemory::bind_uniform_range(uint32_t offset, uint32_t bytes) {
  if (!m_uniforms || (offset & (UniformAlignment - 1)) || uint64_t(offset) + bytes > m_uniformBytes)
    throw std::runtime_error("Native uniform range exceeds active draw slot");
  m_lastUse = m_device.current_fence();
  auto* context = m_device.context();
  const void* uniforms = static_cast<const char*>(m_uniforms) + offset;
  // Native shaders consume BUFFER[0] and direct vertex attributes. Recorded
  // GX raw word bindings 1/2 have no native consumer. No payload copy.
  check(sceGxmSetVertexUniformBuffer(context, 0, uniforms), "bind vertex uniforms");
  check(sceGxmSetFragmentUniformBuffer(context, 0, uniforms), "bind fragment uniforms");
}
void DrawMemory::draw(uint32_t indexCount, uint32_t indexOffset, uint32_t instances) {
  if (!indexCount || !instances)
    return;
  if (!m_indices || (indexOffset & (IndexAlignment - 1)) || uint64_t(indexOffset) + uint64_t(indexCount) * sizeof(uint16_t) > m_indexBytes || instances > 65536 ||
      uint64_t(indexCount) * instances > std::numeric_limits<uint32_t>::max())
    throw std::runtime_error("Native indexed draw exceeds active draw slot");
  m_lastUse = m_device.current_fence();
  auto* context = m_device.context();
  check(sceGxmSetVertexStream(context, 0, m_ids.data()), "bind vertex IDs");
  check(sceGxmSetVertexStream(context, 1, m_ids.data()), "bind instance IDs");
  const void* indices = static_cast<const char*>(m_indices) + indexOffset;
  if (instances == 1)
    check(sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, indexCount), "draw recorded GX indices");
  else
    check(sceGxmDrawInstanced(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, indexCount * instances, indexCount), "draw recorded GX instances");
}
SceGxmNotification DrawMemory::completion_fence() {
  if (m_lastUse.address)
    m_device.end();
  return m_lastUse;
}
SceGxmNotification draw_memory_fence() {
  if (!g_draw)
    throw std::runtime_error("Native draw memory not initialized");
  return g_draw->completion_fence();
}
void bind_draw_memory(const void* uniforms, size_t uniformBytes, const void* indices, size_t indexBytes, const void* vertices, size_t vertexBytes, const void* attributes, size_t attributeBytes) {
  if (!g_draw)
    throw std::runtime_error("Native draw memory not initialized");
  g_draw->bind(uniforms, uniformBytes, indices, indexBytes, vertices, vertexBytes, attributes, attributeBytes);
}
} // namespace aurora::gxm
