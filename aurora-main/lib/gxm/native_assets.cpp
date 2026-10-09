#include "native_assets.hpp"
#include "texture.hpp"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace aurora::gxm {
namespace {
void read(int file, void* memory, uint64_t offset, uint32_t size) {
  auto* destination = static_cast<uint8_t*>(memory);
  for (uint32_t done = 0; done < size;) {
    const auto amount = std::min(size - done, 1u << 20);
    const int count = sceIoPread(file, destination + done, amount, offset + done);
    if (count <= 0)
      throw std::runtime_error("Native texture pack read failed");
    done += count;
  }
}
} // namespace
NativeAssets::~NativeAssets() {
  if (m_file >= 0)
    sceIoClose(m_file);
}
void NativeAssets::open(const char* path) {
  if (m_file >= 0)
    throw std::runtime_error("Native texture pack already open");
  const int file = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (file < 0)
    throw std::runtime_error("Cannot open native texture pack");
  try {
    SceIoStat stat{};
    if (sceIoGetstatByFd(file, &stat) < 0)
      throw std::runtime_error("Cannot stat native texture pack");
    std::array<uint32_t, 8> header{};
    read(file, header.data(), 0, sizeof(header));
    if (header[0] != 0x5456334d || (header[1] != 1 && header[1] != 2) || !header[2] || header[2] > 32768 || header[3] != (header[1] == 1 ? 40u : 64u) || header[4] || header[5] || header[6] ||
        header[7])
      throw std::runtime_error("Invalid native texture pack header");
    m_version = header[1];
    m_records.resize(header[2]);
    const uint64_t dataStart = 32 + uint64_t(m_records.size()) * header[3];
    if (dataStart > stat.st_size)
      throw std::runtime_error("Native texture table exceeds file");
    std::vector<uint8_t> table(m_records.size() * header[3]);
    read(file, table.data(), 32, table.size());
    uint64_t previousEnd = dataStart;
    for (unsigned i = 0; i < m_records.size(); ++i) {
      auto& r = m_records[i];
      r = gfx::decode_native_texture_record({table.data() + i * header[3], header[3]}, m_version);
      gfx::validate_native_texture_record(r, m_version, i + 1, previousEnd, stat.st_size);
      previousEnd = r.offset + r.bytes;
    }
    m_file = file;
    std::fprintf(stderr, "Aurora GXM: opened %u immutable native texture assets\n", unsigned(m_records.size()));
  } catch (...) {
    sceIoClose(file);
    m_records.clear();
    throw;
  }
}
const NativeAssets::Record& NativeAssets::record(uint32_t id) const {
  if (!id || id > m_records.size())
    throw std::runtime_error("Unknown native texture asset ID");
  return m_records[id - 1];
}
gl::NativeTextureInfo NativeAssets::info(uint32_t id) const {
  const auto& r = record(id);
  return {gfx::native_texture_format(r.format),
          r.width,
          r.height,
          r.mips,
          r.bytes,
          bool(r.flags & 1),
          r.image_bytes,
          r.palette_offset,
          r.palette_bytes,
          r.source_palette_delta,
          r.source_palette_entries,
          r.source_palette_format,
          r.source_format,
          m_version == 2};
}
void NativeAssets::load(uint32_t id, TextureStorage& texture) const {
  const auto& r = record(id);
  if (texture.width != r.width || texture.height != r.height || texture.mips != r.mips || texture.format != gfx::native_texture_format(r.format) || texture.size() != r.bytes || !texture.in_cdram() ||
      texture.has_gpu_use() || (m_version == 2 && (texture.image_bytes() != r.image_bytes || texture.palette_offset() != r.palette_offset || texture.palette_bytes() != r.palette_bytes)))
    throw std::runtime_error("Native texture payload does not match CDRAM layout");
  // New allocation: no preceding scene refers to these bytes. The queued
  // upload runs before any draw consumes them, with no finish or staging copy.
  read(m_file, texture.pixels(), r.offset, r.bytes);
}
} // namespace aurora::gxm
