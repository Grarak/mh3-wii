#pragma once
#include "device.hpp"
#include "../gfx/format.hpp"
#include "../gfx/native_texture_layout.hpp"
#include "residency.hpp"
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
  void collect();
  size_t main_used() const noexcept;
  size_t phycont_used() const noexcept;
  ResidencyStatistics statistics() const noexcept;
  uint64_t generation() const noexcept { return m_generation; }

private:
  friend class TextureStorage;
  void allocate(TextureStorage& texture);
  bool place(TextureStorage& texture);
  void release(TextureStorage& texture) noexcept;
  void* allocate_spill(size_t bytes, MappedArena*& arena);
  void move(TextureStorage& texture, void* data, MappedArena& arena);
  struct RetiredAllocation {
    SceGxmNotification fence;
    void* data;
    MappedArena* arena;
    size_t bytes;
  };

  std::vector<RetiredAllocation> m_retired;
  Device& m_device;
  size_t m_mainBudget, m_phycontBudget;
  std::unique_ptr<MappedArena> m_main, m_phycont;
  std::list<TextureStorage*> m_lru;
  uint64_t m_generation = 0;
};

class TextureStorage {
public:
  // samples 2 or 4 (RGBA8 and Depth32F render targets only): an MSAA
  // target. The colour surface downscales into the texture's own pixels; the
  // depth keeps every sample, as a 2x wide (and, at 4x, 2x high) image that
  // sampling reads.
  TextureStorage(TexturePool& pool, uint32_t width, uint32_t height, uint32_t mips, gl::Format format, uint32_t samples = 1);
  ~TextureStorage();
  TextureStorage(const TextureStorage&) = delete;
  TextureStorage& operator=(const TextureStorage&) = delete;
  void upload(uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* source, uint32_t sourcePitch);
  const SceGxmTexture& sampled() const noexcept { return m_sampled; }
  const SceGxmColorSurface* color_surface() const;

  // The colour as a single-sampled surface (a multisampled target's resolved
  // pixels; the colour surface itself otherwise).
  const SceGxmColorSurface* resolved_color_surface() const;
  const SceGxmDepthStencilSurface* depth_surface() const;

  // The depth's memory as a single-sampled surface that loads and stores
  // only in partial renders (a multisampled target's; the depth surface
  // otherwise). Its contents replace the depth.
  const SceGxmDepthStencilSurface* single_depth_surface() const;
  SceGxmOutputRegisterFormat output_format() const noexcept;
  bool in_cdram() const noexcept;
  bool has_gpu_use() const noexcept { return m_lastUse.address || m_gpuWritten; }
  bool gpu_idle() const noexcept { return !aurora::gpu_notification_pending(m_lastUse.address, m_lastUse.value); }
  void pin(bool pinned) noexcept { m_pinned = pinned; }
  void note_gpu_use(bool writes = false) noexcept {
    m_lastUse = m_pool.m_device.current_fence();
    m_gpuWritten |= writes;
  }

  void* pixels() const noexcept { return m_data; }
  uint32_t row_bytes(uint32_t mip = 0) const;
  size_t size() const noexcept { return m_bytes; }
  size_t image_bytes() const noexcept { return m_imageBytes; }
  size_t palette_offset() const noexcept { return m_paletteOffset; }
  size_t palette_bytes() const noexcept { return m_paletteBytes; }
  const uint32_t width, height, mips;
  const gl::Format format;
  const uint32_t samples;

private:
  friend class TexturePool;
  using Level = gfx::NativeTextureLevel;
  void refresh_views();
  TexturePool& m_pool;
  void* m_data = nullptr;
  MappedArena* m_arena = nullptr;
  size_t m_bytes = 0;
  size_t m_imageBytes = 0, m_paletteOffset = 0, m_paletteBytes = 0;
  uint32_t m_pixelBytes = 0;
  bool m_pinned = false, m_linked = false, m_gpuWritten = false;
  SceGxmNotification m_lastUse{};
  std::list<TextureStorage*>::iterator m_lru;
  std::vector<Level> m_levels;
  SceGxmTexture m_sampled{};
  SceGxmColorSurface m_color{}, m_resolved{};
  SceGxmDepthStencilSurface m_depth{}, m_single{};
};

// Native texture control words allow MIRROR on LINEAR textures. These bits
// bypass libgxm's stricter policy; they do not change the memory layout.
void set_texture_address_modes(SceGxmTexture& texture, uint32_t u, uint32_t v) noexcept;
} // namespace aurora::gxm
