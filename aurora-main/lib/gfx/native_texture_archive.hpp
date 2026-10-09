#pragma once
// Standard file I/O is used by host pack validation only. GXM NativeAssets uses
// sceIoPread directly into the CDRAM image+palette allocation instead.
#include "native_texture_pack.hpp"
#include <fstream>

namespace aurora::gfx {
class NativeTextureArchive {
public:
  void open(const char* path) {
    if (!records.empty())
      throw std::runtime_error("Native texture archive already open");
    file.open(path, std::ios::binary | std::ios::ate);
    if (!file)
      throw std::runtime_error("Cannot open native texture archive");
    const auto length = file.tellg();
    if (length < 32)
      throw std::runtime_error("Truncated native texture archive");
    size = uint64_t(length);
    std::array<uint8_t, 32> header{};
    read(header.data(), 0, header.size());
    version = pack_u32(header.data() + 4);
    const auto count = pack_u32(header.data() + 8), stride = pack_u32(header.data() + 12);
    if (pack_u32(header.data()) != 0x5456334d || (version != 1 && version != 2) || !count || count > 32768 || stride != (version == 1 ? 40u : 64u) || pack_u32(header.data() + 16) ||
        pack_u32(header.data() + 20) || pack_u32(header.data() + 24) || pack_u32(header.data() + 28) || 32 + uint64_t(count) * stride > size)
      throw std::runtime_error("Invalid native texture archive header");
    uint64_t end = 32 + uint64_t(count) * stride;
    std::array<uint8_t, 64> bytes{};
    for (uint32_t i = 0; i < count; i++) {
      read(bytes.data(), 32 + uint64_t(i) * stride, stride);
      auto record = decode_native_texture_record({bytes.data(), stride}, version);
      validate_native_texture_record(record, version, i + 1, end, size);
      end = record.offset + record.bytes;
      records.push_back(record);
    }
  }
  const NativeTextureRecord& record(uint32_t id) const {
    if (!id || id > records.size())
      throw std::runtime_error("Unknown native texture archive asset");
    return records[id - 1];
  }
  std::vector<uint8_t> payload(uint32_t id) {
    const auto& r = record(id);
    std::vector<uint8_t> bytes(r.bytes);
    read(bytes.data(), r.offset, bytes.size());
    return bytes;
  }
  uint32_t pack_version() const { return version; }
  size_t count() const { return records.size(); }

private:
  void read(void* out, uint64_t offset, size_t bytes) {
    if (offset > size || bytes > size - offset)
      throw std::runtime_error("Native texture archive read exceeds file");
    file.seekg(offset);
    file.read(static_cast<char*>(out), bytes);
    if (!file)
      throw std::runtime_error("Native texture archive read failed");
  }
  uint64_t size = 0;
  uint32_t version = 0;
  std::ifstream file;
  std::vector<NativeTextureRecord> records;
};
} // namespace aurora::gfx
