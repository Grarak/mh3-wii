#include "texture.hpp"
#include <psp2/kernel/sysmem.h>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>

extern "C" void sceGxmDepthStencilSurfaceSetBackgroundMask(SceGxmDepthStencilSurface*, SceBool);

namespace aurora::gxm {
namespace {
constexpr uint32_t align(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}
struct NativeFormat {
  uint32_t bytes;
  SceGxmTextureFormat sampled;
  SceGxmColorFormat color;
};
NativeFormat native_format(gl::Format format) {
  switch (format) {
  case gl::Format::RGBA8: return {4, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR};
  case gl::Format::R8: return {1, SCE_GXM_TEXTURE_FORMAT_U8_R, SCE_GXM_COLOR_FORMAT_U8_R};
  case gl::Format::RG8: return {2, SCE_GXM_TEXTURE_FORMAT_U8U8_GR, SCE_GXM_COLOR_FORMAT_U8U8_GR};
  // Native S16 sampling is normalized. Store palette indices as exact F32
  // values, which the offline Cg converts to integer indices without loss.
  case gl::Format::R16I:
  case gl::Format::R32F: return {4, SCE_GXM_TEXTURE_FORMAT_F32_R, SCE_GXM_COLOR_FORMAT_F32_R};
  case gl::Format::Depth32F: return {4, SCE_GXM_TEXTURE_FORMAT_F32M_R, SCE_GXM_COLOR_FORMAT_F32_R};
  case gl::Format::BC1: return {0, SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR};
  case gl::Format::BC3: return {0, SCE_GXM_TEXTURE_FORMAT_UBC3_ABGR, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR};
  case gl::Format::R32UI: throw std::runtime_error("Native raw word buffers must use mapped BUFFER bindings");
  }
  throw std::runtime_error("Unknown native texture format");
}
}
TexturePool::TexturePool(Device& device, size_t mainBudget, size_t phycontBudget)
    : m_device(device), m_mainBudget(mainBudget & ~size_t(4095)),
      m_phycontBudget(std::min(phycontBudget, size_t(24u << 20)) & ~size_t((1u << 20) - 1)) {
  if (m_mainBudget > 32u << 20) throw std::runtime_error("Native texture main-RAM spill exceeds 32 MiB budget");
}
TexturePool::~TexturePool() { m_device.finish(); collect(); }
void TexturePool::collect() {
  for (auto it = m_retired.begin(); it != m_retired.end();) {
    if (int32_t(*it->fence.address - it->fence.value) < 0) { ++it; continue; }
    it->arena->free(it->data, it->bytes);
    it = m_retired.erase(it);
  }
}
size_t TexturePool::main_used() const noexcept { return m_main ? m_main->used() : 0; }
size_t TexturePool::phycont_used() const noexcept { return m_phycont ? m_phycont->used() : 0; }
void* TexturePool::allocate_spill(size_t bytes, MappedArena*& arena) {
  if (!m_phycont && m_phycontBudget && bytes <= m_phycontBudget) {
    SceKernelFreeMemorySizeInfo freeMemory{};
    freeMemory.size = sizeof(freeMemory);
    check(sceKernelGetFreeMemorySize(&freeMemory), "query texture spill partitions");
    const size_t available = freeMemory.size_phycont > 2u << 20
        ? (size_t(freeMemory.size_phycont) - (2u << 20)) & ~size_t((1u << 20) - 1) : 0;
    if (available) m_phycont = std::make_unique<MappedArena>(std::min(available, m_phycontBudget), MappedMemory::Kind::Phycont);
  }
  if (m_phycont) {
    if (void* data = m_phycont->allocate(bytes)) { arena = m_phycont.get(); return data; }
  }
  if (!m_main && m_mainBudget && bytes <= m_mainBudget)
    m_main = std::make_unique<MappedArena>(m_mainBudget, MappedMemory::Kind::Draw);
  if (m_main) {
    if (void* data = m_main->allocate(bytes)) { arena = m_main.get(); return data; }
  }
  return nullptr;
}
void TexturePool::move(TextureStorage& texture, void* data, MappedArena& arena) {
  // Earlier scenes hold the old address in their PDS state. Wait before
  // recycling it, then rebuild all views that contain the backing address.
  m_device.finish();
  std::memcpy(data, texture.m_data, texture.m_bytes);
  texture.m_arena->free(texture.m_data, texture.m_bytes);
  texture.m_data = data;
  texture.m_arena = &arena;
  texture.refresh_views();
  ++m_generation;
}
void TexturePool::allocate(TextureStorage& texture) {
  collect();
  auto& cdram = m_device.arena();
  texture.m_data = cdram.allocate(texture.m_bytes);
  while (!texture.m_data) {
    TextureStorage* candidate = nullptr;
    MappedArena* spill = nullptr;
    void* data = nullptr;
    // A large cold target may not fit either spill partition. Keep scanning:
    // moving smaller cold allocations can still make room for the new target.
    for (auto* entry : m_lru) {
      if (entry->m_arena != &cdram || entry->m_pinned) continue;
      if ((data = allocate_spill(entry->m_bytes, spill))) { candidate = entry; break; }
    }
    if (!data) break;
    move(*candidate, data, *spill);
    texture.m_data = cdram.allocate(texture.m_bytes);
  }
  texture.m_arena = &cdram;
  if (!texture.m_data) texture.m_data = allocate_spill(texture.m_bytes, texture.m_arena);
  if (!texture.m_data) {
    std::fprintf(stderr, "Aurora GXM CDRAM exhausted: request %u bytes, arena %u/%u bytes, %u textures\n",
        unsigned(texture.m_bytes), unsigned(cdram.used()), unsigned(cdram.capacity()), unsigned(m_lru.size()));
    std::vector<TextureStorage*> largest(m_lru.begin(), m_lru.end());
    std::sort(largest.begin(), largest.end(), [](auto* a, auto* b) { return a->size() > b->size(); });
    for (size_t i = 0; i < std::min(largest.size(), size_t(12)); ++i) {
      const auto& entry = *largest[i];
      std::fprintf(stderr, "Aurora GXM resident: %ux%u mips %u format %u, %u bytes, pinned %u\n",
          entry.width, entry.height, entry.mips, unsigned(entry.format), unsigned(entry.size()), entry.m_pinned);
    }
    throw std::runtime_error("Native texture residency exhausted CDRAM and bounded spill arenas");
  }
  m_lru.push_back(&texture);
  texture.m_lru = std::prev(m_lru.end());
  texture.m_linked = true;
}
void TexturePool::release(TextureStorage& texture) noexcept {
  if (texture.m_linked) m_lru.erase(texture.m_lru);
  if (texture.m_data) {
    if (texture.m_lastUse.address && int32_t(*texture.m_lastUse.address - texture.m_lastUse.value) < 0) {
      // Close an active consuming scene so its fragment notification will fire.
      m_device.end();
      m_retired.push_back({texture.m_lastUse, texture.m_data, texture.m_arena, texture.m_bytes});
    } else texture.m_arena->free(texture.m_data, texture.m_bytes);
  }
  texture.m_data = nullptr;
  texture.m_linked = false;
}
void TexturePool::touch(TextureStorage& texture) {
  m_lru.splice(m_lru.end(), m_lru, texture.m_lru);
  if (!texture.in_cdram()) {
    if (void* data = m_device.arena().allocate(texture.m_bytes)) move(texture, data, m_device.arena());
  }
}
TextureStorage::TextureStorage(TexturePool& pool, uint32_t width_, uint32_t height_, uint32_t mips_, gl::Format format_)
    : width(width_), height(height_), mips(mips_), format(format_), m_pool(pool) {
  if (!width || !height || width > 4096 || height > 4096 || !mips || mips > 13 ||
      (mips > 1 && (1u << (mips - 1)) > std::max(width, height)) ||
      (format == gl::Format::Depth32F && mips != 1))
    throw std::runtime_error("Invalid native texture dimensions or mip count");
  m_pixelBytes = native_format(format).bytes;
  const bool compressed = format == gl::Format::BC1 || format == gl::Format::BC3;
  if (compressed && (!std::has_single_bit(width) || !std::has_single_bit(height)))
    throw std::runtime_error("Native block-compressed assets require power-of-two dimensions");
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const uint32_t w = std::max(width >> mip, 1u), h = std::max(height >> mip, 1u);
    if (compressed) {
      const uint32_t blocks = std::max(w / 4, 1u) * std::max(h / 4, 1u);
      m_levels.push_back({m_bytes, w, h, 0});
      m_bytes += size_t(blocks) * (format == gl::Format::BC1 ? 8 : 16);
      continue;
    }
    // Depth exports use a tile-aligned stride even with LINEAR storage.
    const uint32_t pitch = align(w, format == gl::Format::Depth32F ? 32 : 8) * m_pixelBytes;
    m_levels.push_back({m_bytes, w, h, pitch});
    if (format == gl::Format::Depth32F) m_bytes += size_t(pitch) * align(h, 32);
    else if (mip + 1 < mips) {
      // Mip rows use the actual width, but the address of the next level
      // advances by the enclosing power-of-two dimensions (GPU guide p.43).
      m_bytes += size_t(std::max(std::bit_ceil(width) >> mip, 8u)) *
                 std::max(std::bit_ceil(height) >> mip, 1u) * m_pixelBytes;
    } else m_bytes += size_t(pitch) * h;
  }
  try {
    m_pool.allocate(*this);
    if (format == gl::Format::Depth32F) {
      // DF32M stores the coverage mask in bit 31. Force-load surfaces must
      // start with mask=1, otherwise every initial draw is rejected.
      std::fill_n(static_cast<uint32_t*>(m_data), m_bytes / 4, 0xbf800000u);
    } else std::memset(m_data, 0, m_bytes);
    refresh_views();
  } catch (...) { m_pool.release(*this); throw; }
}
TextureStorage::~TextureStorage() { m_pool.release(*this); }
bool TextureStorage::in_cdram() const noexcept { return m_arena == &m_pool.m_device.arena(); }
uint32_t TextureStorage::row_bytes(uint32_t mip) const { return m_levels.at(mip).pitch; }
void TextureStorage::refresh_views() {
  const auto native = native_format(format);
  if (format == gl::Format::BC1 || format == gl::Format::BC3) {
    check(sceGxmTextureInitSwizzled(&m_sampled, m_data, native.sampled, width, height, mips), "initialize native compressed asset");
  } else if (format == gl::Format::Depth32F) {
    check(sceGxmTextureInitLinearStrided(&m_sampled, m_data, native.sampled, width, height, m_levels[0].pitch), "initialize sampled depth view");
    check(sceGxmDepthStencilSurfaceInit(&m_depth, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M,
        SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR, m_levels[0].pitch / 4, m_data, nullptr), "initialize native depth surface");
    sceGxmDepthStencilSurfaceSetForceStoreMode(&m_depth, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
    sceGxmDepthStencilSurfaceSetForceLoadMode(&m_depth, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
    sceGxmDepthStencilSurfaceSetBackgroundMask(&m_depth, SCE_TRUE);
  } else {
    check(sceGxmTextureInitLinear(&m_sampled, m_data, native.sampled, width, height, mips), "initialize native linear texture");
    check(sceGxmColorSurfaceInit(&m_color, native.color, SCE_GXM_COLOR_SURFACE_LINEAR,
        SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
        width, height, m_levels[0].pitch / m_pixelBytes, m_data), "initialize native color surface");
  }
}
const SceGxmColorSurface* TextureStorage::color_surface() const {
  if (format == gl::Format::Depth32F || format == gl::Format::BC1 || format == gl::Format::BC3)
    throw std::runtime_error("Sample-only texture used as color target");
  return &m_color;
}
const SceGxmDepthStencilSurface* TextureStorage::depth_surface() const {
  if (format != gl::Format::Depth32F) throw std::runtime_error("Color texture used as depth target");
  return &m_depth;
}
SceGxmOutputRegisterFormat TextureStorage::output_format() const noexcept {
  return format == gl::Format::R32F || format == gl::Format::R16I
      ? SCE_GXM_OUTPUT_REGISTER_FORMAT_FLOAT : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4;
}
void TextureStorage::upload(uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            const void* source, uint32_t sourcePitch) {
  if (!w || !h) return;
  if (format == gl::Format::BC1 || format == gl::Format::BC3)
    throw std::runtime_error("Compressed assets require native packed uploads");
  const auto& level = m_levels.at(mip);
  const uint32_t sourcePixelBytes = format == gl::Format::R16I ? 2 : m_pixelBytes;
  if (!source || uint64_t(x) + w > level.width || uint64_t(y) + h > level.height ||
      uint64_t(w) * sourcePixelBytes > sourcePitch)
    throw std::runtime_error("Native texture upload exceeds level bounds");
  if (m_lastUse.address && int32_t(*m_lastUse.address - m_lastUse.value) < 0) {
    m_pool.m_device.end();
    if (m_gpuWritten) {
      // Debug/readback uploads that preserve GPU-written pixels need their
      // producer completed. The game has no CPU framebuffer composition path.
      m_pool.m_device.finish();
    } else {
      // Mutable CPU textures keep the prior frame's backing until its scene
      // completes; subsequent draws immediately use the replacement backing.
      TextureStorage replacement(m_pool, width, height, mips, format);
      std::memcpy(replacement.m_data, m_data, m_bytes);
      std::swap(m_data, replacement.m_data);
      std::swap(m_arena, replacement.m_arena);
      replacement.m_lastUse = m_lastUse;
      m_lastUse = {};
      refresh_views();
    }
  }
  auto* dest = static_cast<char*>(m_data) + level.offset + size_t(y) * level.pitch + x * m_pixelBytes;
  auto* data = static_cast<const char*>(source);
  for (unsigned row = 0; row < h; ++row) {
    if (format == gl::Format::R16I) {
      for (unsigned col = 0; col < w; ++col) {
        int16_t index;
        std::memcpy(&index, data + col * 2, 2);
        const float value = index;
        std::memcpy(dest + col * 4, &value, 4);
      }
    } else if (format == gl::Format::Depth32F) {
      for (unsigned col = 0; col < w; ++col) {
        uint32_t value;
        std::memcpy(&value, data + col * 4, 4);
        value |= 0x80000000u;
        std::memcpy(dest + col * 4, &value, 4);
      }
    } else std::memcpy(dest, data, size_t(w) * m_pixelBytes);
    dest += level.pitch;
    data += sourcePitch;
  }
}
void set_texture_address_modes(SceGxmTexture& texture, uint32_t u, uint32_t v) noexcept {
  uint32_t word;
  std::memcpy(&word, &texture, 4);
  word = (word & ~0x1f8u) | ((u & 7u) << 6) | ((v & 7u) << 3);
  std::memcpy(&texture, &word, 4);
}
}
