#pragma once
// GXM runtime shader cache pack (ux0:/data/mh3-wii/shaders/cache.pack): every
// compiled program as one appended record, read in one pass at the first
// lookup, so a cached program costs no file operation. Record: magic, stage,
// program hash, source hash (XXH64 of cache version and final Cg), size,
// reserved, then the GXP bytes padded to 4. Parsing stops at the first
// malformed or truncated record; a later record replaces an earlier one with
// the same program hash and stage. Host-testable: no GXM dependency.
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace aurora::gxm::shader_pack {
inline constexpr uint32_t Magic = 0x50534d4d; // "MMSP"
struct Record {
  uint32_t magic, stage;
  uint64_t hash, source;
  uint32_t size, reserved;
};

static_assert(sizeof(Record) == 32);

// (program hash, stage) -> (source hash, GXP bytes)
using Entries = std::map<std::pair<uint64_t, uint32_t>, std::pair<uint64_t, std::string>>;

inline Entries parse(std::string_view bytes, uint32_t maxStage, uint32_t maxProgramBytes) {
  Entries entries;
  size_t at = 0;

  while (bytes.size() - at >= sizeof(Record)) {
    Record record;
    std::memcpy(&record, bytes.data() + at, sizeof record);
    const size_t padded = (size_t(record.size) + 3) & ~size_t(3);

    if (record.magic != Magic || record.stage > maxStage || record.size > maxProgramBytes || padded > bytes.size() - at - sizeof record)
      break;

    entries[{record.hash, record.stage}] = {record.source, std::string(bytes.substr(at + sizeof record, record.size))};
    at += sizeof record + padded;
  }

  return entries;
}

// The bytes appended for one program.
inline std::string encode(uint64_t hash, uint32_t stage, uint64_t source, const void* program, size_t size) {
  const Record record{Magic, stage, hash, source, uint32_t(size), 0};
  std::string out(reinterpret_cast<const char*>(&record), sizeof record);
  out.append(static_cast<const char*>(program), size);
  out.append(((size + 3) & ~size_t(3)) - size, '\0');
  return out;
}
} // namespace aurora::gxm::shader_pack
