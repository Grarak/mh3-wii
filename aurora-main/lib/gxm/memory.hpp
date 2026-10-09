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
  // Main: uncached main memory (4 KiB pages), for GPU data CDRAM cannot hold.
  enum class Kind { Draw, Cdram, Phycont, VertexUsse, FragmentUsse, Main };
  MappedMemory() = default;
  MappedMemory(size_t bytes, Kind kind, const char* name);

  // The same, except that a failed allocation leaves it empty (data() null,
  // the failure in `error`) instead of stopping: for callers with a fallback.
  static MappedMemory try_allocate(size_t bytes, Kind kind, const char* name, int* error = nullptr) noexcept;
  ~MappedMemory();
  MappedMemory(const MappedMemory&) = delete;
  MappedMemory& operator=(const MappedMemory&) = delete;
  MappedMemory(MappedMemory&& other) noexcept;
  MappedMemory& operator=(MappedMemory&& other) noexcept;
  void* data() const noexcept { return m_data; }
  size_t size() const noexcept { return m_size; }
  unsigned usse_offset() const noexcept { return m_usseOffset; }

private:
  int allocate(size_t bytes, Kind kind, const char* name) noexcept;
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
  explicit MappedArena(size_t bytes, MappedMemory::Kind kind = MappedMemory::Kind::Cdram, const char* name = nullptr);
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

// AURORA_MEMBENCH: print the CPU's write speed into each memory kind GPU data
// can live in (CDRAM, uncached main, phycont), with cached main for scale.
void benchmark_cpu_writes() noexcept;
} // namespace aurora::gxm
