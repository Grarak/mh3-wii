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
    if (count <= 0) throw std::runtime_error("Native texture pack read failed");
    done += count;
  }
}
gl::Format format(uint32_t value) {
  switch (value) {
  case 1: return gl::Format::RGBA8;
  case 2: return gl::Format::BC1;
  case 3: return gl::Format::BC3;
  default: throw std::runtime_error("Unsupported offline native texture format");
  }
}
}
NativeAssets::~NativeAssets() { if (m_file >= 0) sceIoClose(m_file); }
void NativeAssets::open(const char* path) {
  if (m_file >= 0) throw std::runtime_error("Native texture pack already open");
  const int file = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (file < 0) throw std::runtime_error("Cannot open native texture pack");
  try {
    SceIoStat stat{};
    if (sceIoGetstatByFd(file, &stat) < 0) throw std::runtime_error("Cannot stat native texture pack");
    std::array<uint32_t, 8> header{};
    read(file, header.data(), 0, sizeof(header));
    if (header[0] != 0x5456334d || header[1] != 1 || !header[2] || header[2] > 32768 || header[3] != sizeof(Record))
      throw std::runtime_error("Invalid native texture pack header");
    m_records.resize(header[2]);
    read(file, m_records.data(), 32, m_records.size() * sizeof(Record));
    const uint64_t dataStart = 32 + m_records.size() * sizeof(Record);
    uint64_t previousEnd = dataStart;
    for (unsigned i = 0; i < m_records.size(); ++i) {
      const auto& r = m_records[i];
      if (r.id != i + 1 || !r.width || !r.height || r.width > 4096 || r.height > 4096 || !r.mips || r.mips > 13 ||
          !r.bytes || r.reserved || r.offset < previousEnd || (r.offset & 63) || r.offset > stat.st_size || r.bytes > stat.st_size - r.offset)
        throw std::runtime_error("Invalid native texture pack record");
      format(r.format);
      previousEnd = r.offset + r.bytes;
    }
    m_file = file;
    std::fprintf(stderr, "Aurora GXM: opened %u immutable native texture assets\n", unsigned(m_records.size()));
  } catch (...) { sceIoClose(file); m_records.clear(); throw; }
}
const NativeAssets::Record& NativeAssets::record(uint32_t id) const {
  if (!id || id > m_records.size()) throw std::runtime_error("Unknown native texture asset ID");
  return m_records[id - 1];
}
gl::NativeTextureInfo NativeAssets::info(uint32_t id) const {
  const auto& r = record(id);
  return {format(r.format), r.width, r.height, r.mips, r.bytes, bool(r.flags & 1)};
}
void NativeAssets::load(uint32_t id, TextureStorage& texture) const {
  const auto& r = record(id);
  if (texture.width != r.width || texture.height != r.height || texture.mips != r.mips ||
      texture.format != format(r.format) || texture.size() != r.bytes || !texture.in_cdram())
    throw std::runtime_error("Native texture payload does not match CDRAM layout");
  // New allocation: no preceding scene refers to these bytes. The queued
  // upload runs before any draw consumes them, with no finish or staging copy.
  read(m_file, texture.pixels(), r.offset, r.bytes);
}
}
