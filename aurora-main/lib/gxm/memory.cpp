#include "memory.hpp"
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace aurora::gxm {
namespace {
size_t aligned(size_t bytes, size_t alignment) {
  if (bytes > SIZE_MAX - alignment + 1) throw std::bad_alloc();
  return (bytes + alignment - 1) & ~(alignment - 1);
}
}

MappedMemory::MappedMemory(size_t bytes, Kind kind, const char* name) : m_kind(kind) {
  m_size = aligned(bytes, kind == Kind::Phycont ? 0x100000 : 0x40000);
  if (m_size == 0) throw std::bad_alloc();
  // All producer buffers, command rings and patched USSE programs live in
  // CDRAM. CPU-only metadata and archival asset bytes use ordinary RAM.
  auto type = kind == Kind::Phycont ? SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW
                                   : SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
  m_block = sceKernelAllocMemBlock(name, type, m_size, nullptr);
  if (m_block < 0) {
    char message[180];
    std::snprintf(message, sizeof(message), "Vita allocation '%s' (%u bytes, type %08x) failed: %08x",
                  name, unsigned(m_size), unsigned(type), unsigned(m_block));
    throw std::runtime_error(message);
  }
  int result = sceKernelGetMemBlockBase(m_block, &m_data);
  if (result >= 0) {
    if (kind == Kind::VertexUsse)
      result = sceGxmMapVertexUsseMemory(m_data, m_size, &m_usseOffset);
    else if (kind == Kind::FragmentUsse)
      result = sceGxmMapFragmentUsseMemory(m_data, m_size, &m_usseOffset);
    else
      result = sceGxmMapMemory(m_data, m_size, static_cast<SceGxmMemoryAttribFlags>(
          SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE));
  }
  if (result < 0) {
    sceKernelFreeMemBlock(m_block);
    m_block = -1;
    m_data = nullptr;
    throw std::runtime_error("Cannot map Vita GPU memory");
  }
  std::fprintf(stderr, "Aurora GXM allocation: %s, %.2f MiB %s at %08x\n", name, m_size / 1048576.0,
               kind == Kind::Phycont ? "phycont" : "CDRAM", unsigned(reinterpret_cast<uintptr_t>(m_data)));
}
MappedMemory::~MappedMemory() { release(); }
MappedMemory::MappedMemory(MappedMemory&& other) noexcept { *this = std::move(other); }
MappedMemory& MappedMemory::operator=(MappedMemory&& other) noexcept {
  if (this == &other) return *this;
  release();
  m_block = std::exchange(other.m_block, -1);
  m_data = std::exchange(other.m_data, nullptr);
  m_size = std::exchange(other.m_size, 0);
  m_kind = other.m_kind;
  m_usseOffset = other.m_usseOffset;
  return *this;
}
void MappedMemory::release() noexcept {
  if (m_block < 0) return;
  if (m_kind == Kind::VertexUsse) sceGxmUnmapVertexUsseMemory(m_data);
  else if (m_kind == Kind::FragmentUsse) sceGxmUnmapFragmentUsseMemory(m_data);
  else sceGxmUnmapMemory(m_data);
  sceKernelFreeMemBlock(m_block);
  m_block = -1;
  m_data = nullptr;
  m_size = 0;
}

MappedArena::MappedArena(size_t bytes, MappedMemory::Kind kind)
    : m_memory(bytes, kind, kind == MappedMemory::Kind::Cdram ? "Aurora CDRAM arena" : "Aurora spill arena") {
  m_free.emplace(0, m_memory.size());
}
void* MappedArena::allocate(size_t bytes) {
  if (bytes == 0) return nullptr;
  bytes = aligned(bytes, 64);
  std::lock_guard lock(m_mutex);
  // Best fit preserves larger spans for the EFB and full-screen copies.
  auto best = m_free.end();
  for (auto it = m_free.begin(); it != m_free.end(); ++it)
    if (it->second >= bytes && (best == m_free.end() || it->second < best->second)) best = it;
  if (best == m_free.end()) return nullptr;
  const auto [offset, length] = *best;
  m_free.erase(best);
  if (length > bytes) m_free.emplace(offset + bytes, length - bytes);
  m_used += bytes;
  m_highWater = std::max(m_highWater, m_used);
  return static_cast<char*>(m_memory.data()) + offset;
}
void MappedArena::free(void* pointer, size_t bytes) noexcept {
  if (!pointer || bytes == 0) return;
  bytes = (bytes + 63) & ~size_t(63);
  size_t offset = static_cast<char*>(pointer) - static_cast<char*>(m_memory.data());
  std::lock_guard lock(m_mutex);
  m_used -= bytes;
  auto next = m_free.lower_bound(offset);
  if (next != m_free.begin()) {
    auto previous = std::prev(next);
    if (previous->first + previous->second == offset) {
      offset = previous->first;
      bytes += previous->second;
      m_free.erase(previous);
    }
  }
  if (next != m_free.end() && offset + bytes == next->first) {
    bytes += next->second;
    m_free.erase(next);
  }
  m_free.emplace(offset, bytes);
}
size_t MappedArena::used() const noexcept { std::lock_guard lock(m_mutex); return m_used; }
size_t MappedArena::high_water() const noexcept { std::lock_guard lock(m_mutex); return m_highWater; }
}
