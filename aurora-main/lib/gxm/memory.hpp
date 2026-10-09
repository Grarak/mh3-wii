#pragma once

#include <psp2/types.h>
#include <cstddef>
#include <map>
#include <mutex>

namespace aurora::gxm {

// CPU writers and GXM read the same allocation. A draw batch must keep this
// memory alive until its last scene has completed; recycling is a renderer action.
class MappedMemory {
public:
  enum class Kind { Draw, Cdram, Phycont, VertexUsse, FragmentUsse };
  MappedMemory() = default;
  MappedMemory(size_t bytes, Kind kind, const char* name);
  ~MappedMemory();
  MappedMemory(const MappedMemory&) = delete;
  MappedMemory& operator=(const MappedMemory&) = delete;
  MappedMemory(MappedMemory&& other) noexcept;
  MappedMemory& operator=(MappedMemory&& other) noexcept;
  void* data() const noexcept { return m_data; }
  size_t size() const noexcept { return m_size; }
  unsigned usse_offset() const noexcept { return m_usseOffset; }

private:
  void release() noexcept;
  SceUID m_block = -1;
  void* m_data = nullptr;
  size_t m_size = 0;
  unsigned m_usseOffset = 0;
  Kind m_kind = Kind::Draw;
};

// One large CDRAM memblock avoids the 256 KiB per-block allocation rounding
// for thousands of small textures. Suballocations satisfy GXM's 64-byte base
// alignment; callers account for row and mip layout before allocating.
class MappedArena {
public:
  explicit MappedArena(size_t bytes, MappedMemory::Kind kind = MappedMemory::Kind::Cdram);
  void* allocate(size_t bytes);
  bool can_allocate(size_t bytes) const noexcept;
  void free(void* pointer, size_t bytes) noexcept;
  size_t used() const noexcept;
  size_t high_water() const noexcept;
  size_t capacity() const noexcept { return m_memory.size(); }

private:
  MappedMemory m_memory;
  mutable std::mutex m_mutex;
  std::map<size_t, size_t> m_free;
  size_t m_used = 0;
  size_t m_highWater = 0;
};
using CdramArena = MappedArena;
} // namespace aurora::gxm
