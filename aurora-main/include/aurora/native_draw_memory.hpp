#pragma once

#include <cstdint>
#include <span>

namespace aurora {
// Borrowed mutable data for this rendered frame only. Three bounded slots
// retain their allocations until shutdown; frame admission protects reuse.
// GXM maps vertices, uniforms and indices directly in CDRAM without uploads
// or payload copies. Its unused recorded-GX attribute-word span is empty.
struct NativeDrawMemory {
  std::span<uint8_t> vertices;
  std::span<uint8_t> uniforms;
  std::span<uint8_t> indices;
  std::span<uint8_t> attributes;
};

// A skipped or inactive frame returns empty spans. Do not retain a writable
// view after its frame ends. Populate draw inputs before issuing their draws.
NativeDrawMemory native_draw_memory() noexcept;
} // namespace aurora
