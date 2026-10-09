#pragma once
#include <aurora/fatal.hpp>
#include "native_texture_layout.hpp"
#include <array>
#include <span>

namespace aurora::gfx {
struct NativeTextureRecord {
  uint32_t id, format, width, height, mips, flags, bytes, palette_offset;
  uint64_t offset;
  uint32_t image_bytes, palette_bytes;
  int32_t source_palette_delta;
  uint32_t source_palette_entries, source_palette_format, source_format;
};

static_assert(sizeof(NativeTextureRecord) == 64);
inline void validate_native_texture_record(const NativeTextureRecord& r, uint32_t version, uint32_t expected_id, uint64_t previous_end, uint64_t file_bytes) {
  if (r.id != expected_id || r.flags & ~3u || !r.bytes || r.offset & 63 || r.offset < previous_end || r.offset > file_bytes || r.bytes > file_bytes - r.offset)
    AURORA_THROW(std::runtime_error("Invalid native texture pack record"));

  const auto format = native_texture_format(r.format);
  const auto layout = native_texture_layout(r.width, r.height, r.mips, format);

  if (version == 1) {
    if (r.format > 3 || r.palette_offset || r.bytes != layout.total_bytes || r.flags & ~1u)
      AURORA_THROW(std::runtime_error("Invalid version1 native texture record"));
  } else if (version == 2) {
    if (r.bytes != layout.total_bytes || r.image_bytes != layout.image_bytes || r.palette_offset != layout.palette_offset || r.palette_bytes != layout.palette_bytes || r.source_format > 14 ||
        r.source_format == 7 || (r.source_format > 10 && r.source_format < 14) || r.source_palette_format > 2 || r.source_palette_entries > 16384 ||
        ((r.source_format == 8 || r.source_format == 9 || r.source_format == 10) != bool(r.source_palette_entries)) || (!r.source_palette_entries && r.source_palette_delta))
      AURORA_THROW(std::runtime_error("Native texture record disagrees with storage/provenance"));
    if (bool(r.flags & 2) != (format == gl::Format::BC4_RRRR || format == gl::Format::BC5_GRRR))
      AURORA_THROW(std::runtime_error("Lossy texture requires an explicit native policy flag"));
  } else
    AURORA_THROW(std::runtime_error("Unsupported native texture pack version"));
}

inline uint32_t pack_u32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

inline NativeTextureRecord decode_native_texture_record(std::span<const uint8_t> bytes, uint32_t version) {
  if (bytes.size() != (version == 1 ? 40u : 64u))
    AURORA_THROW(std::runtime_error("Truncated native texture record"));

  NativeTextureRecord r{};
  uint32_t fields[8];

  for (unsigned i = 0; i < 8; i++)
    fields[i] = pack_u32(bytes.data() + i * 4);

  r.id = fields[0];
  r.format = fields[1];
  r.width = fields[2];
  r.height = fields[3];
  r.mips = fields[4];
  r.flags = fields[5];
  r.bytes = fields[6];
  r.palette_offset = fields[7];
  r.offset = uint64_t(pack_u32(bytes.data() + 32)) | uint64_t(pack_u32(bytes.data() + 36)) << 32;

  if (version == 2) {
    r.image_bytes = pack_u32(bytes.data() + 40);
    r.palette_bytes = pack_u32(bytes.data() + 44);
    r.source_palette_delta = std::bit_cast<int32_t>(pack_u32(bytes.data() + 48));
    r.source_palette_entries = pack_u32(bytes.data() + 52);
    r.source_palette_format = pack_u32(bytes.data() + 56);
    r.source_format = pack_u32(bytes.data() + 60);
  }

  return r;
}
} // namespace aurora::gfx
