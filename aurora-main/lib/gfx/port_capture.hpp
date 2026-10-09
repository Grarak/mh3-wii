#pragma once
#include <cstdint>
#include <array>
#include <string_view>

// Opt-in host measurements for preparing the offline Vita shader corpus.
// Safe on producer and render threads; AURORA_PORT_CAPTURE enables output.
namespace aurora::gfx::port_capture {
bool enabled() noexcept;
// Kept with the recorded draw, so merged batches and render-thread replay
// retain the producer's frame and guest origin.
struct DrawState {
  uint32_t frame = 0;
  uint32_t alphaCompare = 0;
  uint32_t flags = 0;
  uint32_t primitives = 1;
  uint32_t displayList = 0, displayListOffset = 0;
  std::array<uint32_t, 4> callers{};
  // Guest texture addresses on the Wii FIFO path, captured before state changes.
  std::array<uint32_t, 8> textureSources{};
};
enum DrawFlags : uint32_t {
  DepthCompare = 1, DepthWrite = 2, ZBeforeTexture = 4, Blend = 8,
  ColorWrite = 16, AlphaWrite = 32, ZTexture = 64,
  DepthFunctionShift = 8,
};
void draw(const DrawState& state, uint64_t vertexHash, uint64_t fragmentHash,
          uint32_t vertices, uint32_t indices, uint32_t instances,
          bool discard, bool depthReplace) noexcept;
void shader(std::string_view label, std::string_view vertex, std::string_view fragment) noexcept;
void texture(const void* id, bool create, uint32_t width, uint32_t height, uint32_t mips,
             uint32_t format, uint32_t bytesPerPixel, std::string_view label = {}) noexcept;
void upload(const void* id, uint32_t mip, uint64_t bytes) noexcept;
void frame(uint32_t index, uint32_t draws, uint32_t merged, uint32_t passes,
           uint64_t vertices, uint64_t uniforms, uint64_t indices, uint64_t storage) noexcept;
}
