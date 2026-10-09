#pragma once
#include "device.hpp"
#include "../gfx/format.hpp"
#include <list>
#include <vector>

namespace aurora::gxm {
class TextureStorage;

// Texture backing has one live copy. Cold CDRAM allocations may move into
// bounded mapped spill arenas; GXM consumes those allocations directly too.
class TexturePool {
public:
  explicit TexturePool(Device& device, size_t mainBudget = 0, size_t phycontBudget = 0);
  ~TexturePool();
  void touch(TextureStorage& texture);
  size_t main_used() const noexcept;
  size_t phycont_used() const noexcept;
  uint64_t generation() const noexcept { return m_generation; }

private:
  friend class TextureStorage;
  void allocate(TextureStorage& texture);
  void release(TextureStorage& texture) noexcept;
  void* allocate_spill(size_t bytes, MappedArena*& arena);
  void move(TextureStorage& texture, void* data, MappedArena& arena);
  Device& m_device;
  size_t m_mainBudget, m_phycontBudget;
  std::unique_ptr<MappedArena> m_main, m_phycont;
  std::list<TextureStorage*> m_lru;
  uint64_t m_generation = 0;
};

class TextureStorage {
public:
  TextureStorage(TexturePool& pool, uint32_t width, uint32_t height, uint32_t mips, gl::Format format);
  ~TextureStorage();
  TextureStorage(const TextureStorage&) = delete;
  TextureStorage& operator=(const TextureStorage&) = delete;
  void upload(uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
              const void* source, uint32_t sourcePitch);
  const SceGxmTexture& sampled() const noexcept { return m_sampled; }
  const SceGxmColorSurface* color_surface() const;
  const SceGxmDepthStencilSurface* depth_surface() const;
  SceGxmOutputRegisterFormat output_format() const noexcept;
  bool in_cdram() const noexcept;
  void pin(bool pinned) noexcept { m_pinned = pinned; }
  void* pixels() const noexcept { return m_data; }
  uint32_t row_bytes(uint32_t mip = 0) const;
  size_t size() const noexcept { return m_bytes; }
  const uint32_t width, height, mips;
  const gl::Format format;

private:
  friend class TexturePool;
  struct Level { size_t offset; uint32_t width, height, pitch; };
  void refresh_views();
  TexturePool& m_pool;
  void* m_data = nullptr;
  MappedArena* m_arena = nullptr;
  size_t m_bytes = 0;
  uint32_t m_pixelBytes = 0;
  bool m_pinned = false, m_linked = false;
  std::list<TextureStorage*>::iterator m_lru;
  std::vector<Level> m_levels;
  SceGxmTexture m_sampled{};
  SceGxmColorSurface m_color{};
  SceGxmDepthStencilSurface m_depth{};
};

// Native texture control words allow MIRROR on LINEAR textures. These bits
// bypass libgxm's stricter policy; they do not change the memory layout.
void set_texture_address_modes(SceGxmTexture& texture, uint32_t u, uint32_t v) noexcept;
}
