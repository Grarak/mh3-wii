#pragma once

#include <dolphin/gx.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace aurora::gx {

struct TextureBindGroupCacheKey {
  std::uint64_t sampledTextures = 0;
  std::uint64_t sampledIndTextures = 0;
  std::array<const void*, GX_MAX_TEXMAP> textureViews{};
  std::array<std::uint32_t, GX_MAX_TEXMAP> mode0{};
  std::array<std::uint32_t, GX_MAX_TEXMAP> mode1{};
  std::array<std::uint32_t, GX_MAX_TEXMAP> refState{};

  bool operator==(const TextureBindGroupCacheKey&) const = default;
};

inline void set_texture_bind_group_cache_slot(TextureBindGroupCacheKey& key, std::size_t slot,
                                              const void* textureView, std::uint32_t mode0,
                                              std::uint32_t mode1, std::uint32_t refState) noexcept {
  key.textureViews[slot] = textureView;
  key.mode0[slot] = mode0;
  key.mode1[slot] = mode1;
  key.refState[slot] = refState;
}

} // namespace aurora::gx
