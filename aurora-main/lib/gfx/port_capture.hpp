#pragma once
#include <cstdint>
#include <string_view>

// Opt-in host measurements for preparing the offline Vita shader corpus.
// Safe on producer and render threads; AURORA_PORT_CAPTURE enables output.
namespace aurora::gfx::port_capture {
void shader(std::string_view label, std::string_view vertex, std::string_view fragment) noexcept;
void texture(const void* id, bool create, uint32_t width, uint32_t height, uint32_t mips,
             uint32_t format, uint32_t bytesPerPixel, std::string_view label = {}) noexcept;
void upload(const void* id, uint32_t mip, uint64_t bytes) noexcept;
void frame(uint32_t index, uint32_t draws, uint32_t merged, uint32_t passes,
           uint64_t vertices, uint64_t uniforms, uint64_t indices, uint64_t storage) noexcept;
}
