#pragma once
#include "../gl/gl.hpp"
#include <stdexcept>

namespace aurora::gfx {
inline constexpr const char* MeshAttributeNames[] = {
    "a_mesh_position", "a_mesh_normal", "a_mesh_color0", "a_mesh_color1",
    "a_mesh_tex0", "a_mesh_tex1", "a_mesh_tex2", "a_mesh_tex3",
    "a_mesh_tex4", "a_mesh_tex5", "a_mesh_tex6", "a_mesh_tex7",
    "a_mesh_matrix0", "a_mesh_matrix1", "a_mesh_matrix2"};
inline void validate_mesh_layout(const gl::MeshLayout& layout) {
  if (!layout.stride || layout.attributes.empty() || layout.attributes.size() > 15 ||
      (layout.matrixIndexMask & ~0x1ffu))
    throw std::runtime_error("Invalid native mesh layout");
  unsigned used = 0;
  for (const auto& attribute : layout.attributes) {
    if (attribute.location >= 15 || (used & (1u << attribute.location)) ||
        !attribute.components || attribute.components > 4)
      throw std::runtime_error("Invalid native mesh attribute");
    used |= 1u << attribute.location;
    if (attribute.location >= 12) {
      if (attribute.format != gl::MeshAttributeFormat::U8 ||
          attribute.components != (attribute.location == 14 ? 1 : 4))
        throw std::runtime_error("Invalid native mesh matrix selector input");
    } else if (attribute.format == gl::MeshAttributeFormat::U8) {
      throw std::runtime_error("Raw native mesh bytes require a matrix selector input");
    }
    unsigned bytes;
    switch (attribute.format) {
    case gl::MeshAttributeFormat::F32: bytes=4; break;
    case gl::MeshAttributeFormat::F16:
    case gl::MeshAttributeFormat::S16N: bytes=2; break;
    case gl::MeshAttributeFormat::U8N:
    case gl::MeshAttributeFormat::U8: bytes=1; break;
    default: throw std::runtime_error("Unknown native mesh attribute format");
    }
    if (attribute.offset % bytes || attribute.offset + bytes*attribute.components > layout.stride)
      throw std::runtime_error("Native mesh attribute exceeds stride or is misaligned");
  }
  if (!(used & 1)) throw std::runtime_error("Native mesh has no position");
  const unsigned requiredGroups = ((layout.matrixIndexMask & 15) ? 1u << 12 : 0) |
      ((layout.matrixIndexMask & 240) ? 1u << 13 : 0) |
      ((layout.matrixIndexMask & 256) ? 1u << 14 : 0);
  if ((used & 0x7000u) != requiredGroups)
    throw std::runtime_error("Native mesh matrix selector mask differs from packed inputs");
}
inline void validate_mesh_draw(const gl::NativeMeshBuffer& buffer, const gl::MeshLayout& layout,
                               uint32_t vertexOffset, uint32_t vertexCount,
                               uint32_t indexOffset, uint32_t indexCount) {
  validate_mesh_layout(layout);
  // The converted pack loader validates every U16 index once. Do not rescan
  // index contents here; this boundary checks only the changing draw ranges.
  if (!vertexCount || vertexCount > 65536 || indexCount % 3 || vertexOffset % 4 || indexOffset % 2 ||
      uint64_t(vertexOffset) + uint64_t(vertexCount)*layout.stride > buffer.size() ||
      uint64_t(indexOffset) + uint64_t(indexCount)*2 > buffer.size())
    throw std::runtime_error("Native mesh draw exceeds resident payload");
}
}
