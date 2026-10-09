#include "memory.hpp"
#include <aurora/fatal.hpp>
#include "razor.hpp"
#include <psp2/gxm.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/sysmem.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace aurora::gxm {
namespace {
size_t aligned(size_t bytes, size_t alignment) {
  return (bytes + alignment - 1) & ~(alignment - 1);
}

SceKernelMemBlockType memblock_type(MappedMemory::Kind kind) {
  if (kind == MappedMemory::Kind::Phycont)
    return SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW;

  return kind == MappedMemory::Kind::Main ? SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE : SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
}
} // namespace

MappedMemory::MappedMemory(size_t bytes, Kind kind, const char* name) : m_kind(kind) {
  if (const int error = allocate(bytes, kind, name); error < 0) {
    if (error == SCE_KERNEL_ERROR_ERROR) // mapping
      AURORA_THROW(std::runtime_error("Cannot map Vita GPU memory"));

    char message[180];
    std::snprintf(message, sizeof(message), "Vita allocation '%s' (%u bytes, type %08x) failed: %08x", name, unsigned(m_size), unsigned(memblock_type(kind)), unsigned(error));
    AURORA_THROW(std::runtime_error(message));
  }
}

MappedMemory MappedMemory::try_allocate(size_t bytes, Kind kind, const char* name, int* error) noexcept {
  MappedMemory memory;
  memory.m_kind = kind;
  const int result = memory.allocate(bytes, kind, name);

  if (error)
    *error = result;

  return memory;
}

// 0 or the failure: the kernel's allocation error, or SCE_KERNEL_ERROR_ERROR
// when GXM cannot map the block. A failure leaves nothing allocated.
int MappedMemory::allocate(size_t bytes, Kind kind, const char* name) noexcept {
  const size_t alignment = kind == Kind::Phycont ? 0x100000 : kind == Kind::Main ? 0x1000 : 0x40000;

  if (bytes == 0 || bytes > SIZE_MAX - alignment + 1)
    return SCE_KERNEL_ERROR_INVALID_ARGUMENT;

  m_size = aligned(bytes, alignment);

  // All producer buffers, command rings and patched USSE programs live in
  // CDRAM. CPU-only metadata and archival asset bytes use ordinary RAM.
  const SceKernelMemBlockType type = memblock_type(kind);
  m_block = sceKernelAllocMemBlock(name, type, m_size, nullptr);

  if (m_block < 0) {
    const int error = m_block;
    m_block = -1;
    return error;
  }

  int result = sceKernelGetMemBlockBase(m_block, &m_data);

  if (result >= 0) {
    if (kind == Kind::VertexUsse)
      result = sceGxmMapVertexUsseMemory(m_data, m_size, &m_usseOffset);
    else if (kind == Kind::FragmentUsse)
      result = sceGxmMapFragmentUsseMemory(m_data, m_size, &m_usseOffset);
    else
      result = sceGxmMapMemory(m_data, m_size, static_cast<SceGxmMemoryAttribFlags>(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE));
  }

  if (result < 0) {
    sceKernelFreeMemBlock(m_block);
    m_block = -1;
    m_data = nullptr;
    return SCE_KERNEL_ERROR_ERROR;
  }

  razor_register_buffer(m_data, m_size);
  std::fprintf(stderr, "Aurora GXM allocation: %s, %.2f MiB %s at %08x\n", name, m_size / 1048576.0, kind == Kind::Phycont ? "phycont" : kind == Kind::Main ? "main" : "CDRAM",
               unsigned(reinterpret_cast<uintptr_t>(m_data)));
  return 0;
}

MappedMemory::~MappedMemory() {
  release();
}

MappedMemory::MappedMemory(MappedMemory&& other) noexcept {
  *this = std::move(other);
}

MappedMemory& MappedMemory::operator=(MappedMemory&& other) noexcept {
  if (this == &other)
    return *this;

  release();
  m_block = std::exchange(other.m_block, -1);
  m_data = std::exchange(other.m_data, nullptr);
  m_size = std::exchange(other.m_size, 0);
  m_kind = other.m_kind;
  m_usseOffset = other.m_usseOffset;
  return *this;
}

void MappedMemory::release() noexcept {
  if (m_block < 0)
    return;

  razor_unregister_buffer(m_data);

  if (m_kind == Kind::VertexUsse)
    sceGxmUnmapVertexUsseMemory(m_data);
  else if (m_kind == Kind::FragmentUsse)
    sceGxmUnmapFragmentUsseMemory(m_data);
  else
    sceGxmUnmapMemory(m_data);

  sceKernelFreeMemBlock(m_block);
  m_block = -1;
  m_data = nullptr;
  m_size = 0;
}

MappedArena::MappedArena(size_t bytes, MappedMemory::Kind kind) : m_memory(bytes, kind, kind == MappedMemory::Kind::Cdram ? "Aurora CDRAM arena" : "Aurora spill arena") {
  m_free.emplace(0, m_memory.size());
}

void* MappedArena::allocate(size_t bytes) {
  if (bytes == 0)
    return nullptr;

  bytes = aligned(bytes, 64);
  std::lock_guard lock(m_mutex);

  // Best fit preserves larger spans for the EFB and full-screen copies.
  auto best = m_free.end();

  for (auto it = m_free.begin(); it != m_free.end(); ++it)
    if (it->second >= bytes && (best == m_free.end() || it->second < best->second))
      best = it;
  if (best == m_free.end())
    return nullptr;

  const auto [offset, length] = *best;
  m_free.erase(best);

  if (length > bytes)
    m_free.emplace(offset + bytes, length - bytes);

  m_used += bytes;
  m_highWater = std::max(m_highWater, m_used);
  return static_cast<char*>(m_memory.data()) + offset;
}

bool MappedArena::can_allocate(size_t bytes) const noexcept {
  if (!bytes)
    return false;

  bytes = aligned(bytes, 64);
  std::lock_guard lock(m_mutex);

  for (const auto& block : m_free)
    if (block.second >= bytes)
      return true;
  return false;
}

void MappedArena::free(void* pointer, size_t bytes) noexcept {
  if (!pointer || bytes == 0)
    return;

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

size_t MappedArena::used() const noexcept {
  std::lock_guard lock(m_mutex);
  return m_used;
}

size_t MappedArena::high_water() const noexcept {
  std::lock_guard lock(m_mutex);
  return m_highWater;
}
} // namespace aurora::gxm
