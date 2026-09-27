#include "gx/texture_bind_group_cache_key.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <new>

namespace aurora::gx {
namespace {

TEST(TextureBindGroupCacheKeyTest, DistinctViewsDoNotAliasWhenWrapperStorageIsReused) {
  struct SimulatedTextureRef {
    const void* sampleTextureView;
  };

  alignas(SimulatedTextureRef) std::array<std::byte, sizeof(SimulatedTextureRef)> wrapperStorage{};
  int firstView = 0;
  int secondView = 0;

  TextureBindGroupCacheKey first{
      .sampledTextures = 1,
  };
  TextureBindGroupCacheKey second = first;
  auto* firstWrapper = new (wrapperStorage.data()) SimulatedTextureRef{&firstView};
  const void* const reusedWrapperAddress = firstWrapper;
  set_texture_bind_group_cache_slot(first, 0, firstWrapper->sampleTextureView, 0x1234, 0x5678, 0x9abc);
  firstWrapper->~SimulatedTextureRef();

  auto* secondWrapper = new (wrapperStorage.data()) SimulatedTextureRef{&secondView};
  set_texture_bind_group_cache_slot(second, 0, secondWrapper->sampleTextureView, 0x1234, 0x5678, 0x9abc);

  // A freed TextureRef wrapper can be reconstructed at the same allocator
  // address in one frame. The old key therefore saw these bindings as equal.
  EXPECT_EQ(reusedWrapperAddress, static_cast<const void*>(secondWrapper));
  EXPECT_NE(first, second);

  TextureBindGroupCacheKey sameView = first;
  EXPECT_EQ(first, sameView);
  secondWrapper->~SimulatedTextureRef();
}

} // namespace
} // namespace aurora::gx
