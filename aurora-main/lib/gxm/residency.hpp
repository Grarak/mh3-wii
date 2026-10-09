#pragma once

#include <cstdint>

namespace aurora::gxm {
// Owner-thread snapshot of allocations currently held, including backing
// awaiting retirement. This neither submits work nor waits for completion.
struct ResidencyStatistics {
  uint64_t arena_used = 0, arena_capacity = 0;
  uint64_t texture_bytes = 0, retired_texture_bytes = 0;
  uint64_t model_bytes = 0, retired_model_bytes = 0;
  uint64_t main_spill_bytes = 0, phycont_spill_bytes = 0;
  uint32_t textures = 0, pinned_textures = 0, gpu_written_textures = 0;
};

ResidencyStatistics residency_statistics() noexcept;
} // namespace aurora::gxm
