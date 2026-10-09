#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace aurora {
struct MaterialSource {
  uint32_t width = 0, height = 0, format = 0, nativeAsset = 0;
  std::vector<uint8_t> bytes; // host fallback; Vita uses the external native pack
};

struct MaterialComposition {
  // Base, color overlay, optional overlay, small atlas region.
  std::array<MaterialSource, 4> sources;
  float opacity = 1.f;
  bool optionalOverlay = false, atlas = false;
  uint32_t atlasX = 0, atlasY = 0;
};

// Called in GX command order, on the GX producer/worker, not concurrently.
// Each publication renders a fresh texture. Recorded frames retain old owners.
void publish_generated_material(uint32_t owner, const void* guestData, MaterialComposition material);
uint32_t generated_texture_version(const void* guestData, uint32_t width, uint32_t height);
void clear_generated_textures();
} // namespace aurora
