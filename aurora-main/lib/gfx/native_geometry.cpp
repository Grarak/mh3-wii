#include <aurora/native_geometry.h>
#include <aurora/native_draw_memory.hpp>
#include <aurora/aurora.h>
#include "common.hpp"
#include "../gl/gl.hpp"
#include "../internal.hpp"
#include <limits>

namespace {
aurora::Module Log("aurora::native_geometry");
uint32_t frame = UINT32_MAX;
size_t vertexCursor = 0, indexCursor = 0;

template <class Geometry>
int allocate(uint32_t vertices, uint32_t indices, Geometry* out) {
  if (!out) return 0;
  *out = {};
  if (!aurora_native_frame_should_render() || !vertices || !indices) return 0;
  if (!aurora::gl::on_render_thread()) Log.fatal("Geometry allocated outside game thread");
  if (vertices > 65536) return 0;
  const auto current = aurora::gfx::current_frame();
  if (current != frame) { frame = current; vertexCursor = indexCursor = 0; }
  const auto memory = aurora::native_draw_memory();
  const size_t start = (vertexCursor + 15) & ~size_t(15);
  const uint64_t vertexBytes = uint64_t(vertices) * sizeof(*out->vertices);
  const uint64_t indexBytes = uint64_t(indices) * sizeof(uint16_t);
  if (start > memory.vertices.size() || vertexBytes > memory.vertices.size() - start ||
      indexCursor > memory.indices.size() || indexBytes > memory.indices.size() - indexCursor)
    return 0;
  *out = {reinterpret_cast<decltype(out->vertices)>(memory.vertices.data() + start),
          reinterpret_cast<uint16_t*>(memory.indices.data() + indexCursor),
          vertices, indices, current, aurora_native_frame_slot()};
  vertexCursor = start + size_t(vertexBytes);
  indexCursor += size_t(indexBytes);
  return 1;
}
}

extern "C" int aurora_allocate_native_geometry(uint32_t vertices, uint32_t indices,
                                               AuroraNativeGeometry* out) {
  return allocate(vertices, indices, out);
}
extern "C" int aurora_allocate_native_material_geometry(uint32_t vertices, uint32_t indices,
                                                        AuroraNativeMaterialGeometry* out) {
  return allocate(vertices, indices, out);
}

extern "C" void aurora_submit_native_geometry(const AuroraNativeGeometry* geometry,
                                              int texture_modulate) {
  if (!aurora_native_frame_should_render()) return;
  if (!geometry || geometry->frame != aurora::gfx::current_frame() ||
      geometry->slot != aurora_native_frame_slot())
    Log.fatal("Geometry submitted outside its owning frame");
  if (!aurora::gl::on_render_thread()) Log.fatal("Geometry submitted outside game thread");
  const auto memory = aurora::native_draw_memory();
  const auto valid = [](const void* p, uint64_t bytes, const auto& span) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(p), begin = reinterpret_cast<uintptr_t>(span.data());
    return at >= begin && at - begin <= span.size() && bytes <= span.size() - (at - begin);
  };
  if (!valid(geometry->vertices, uint64_t(geometry->vertex_count) * sizeof(AuroraNativeVertex), memory.vertices) ||
      !valid(geometry->indices, uint64_t(geometry->index_count) * sizeof(uint16_t), memory.indices))
    Log.fatal("Geometry exceeds admitted draw memory");
  aurora::gl::draw_native_geometry(*geometry, texture_modulate != 0);
}
