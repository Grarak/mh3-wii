#pragma once
// Development fallback only (MH3_DEVELOPMENT): compiles, with vitaShaRK on the
// device, a program the offline shader bank lacks and reports the miss.
// Release builds have no runtime compiler (docs/optimizations/rendering/offline-shader-bank.md).
#include "shader_bank.hpp"
#include <psp2/gxm.h>
#include <cstdint>
#include <string_view>

namespace aurora::gxm {
using RuntimeShaderStage = ShaderStage;

// Called on the graphics owner with the program's sources; retained only for
// stages the bank lacks. `cg` sources are hand-written Cg and compile without
// GLSL conversion.
void register_runtime_shader_sources(uint64_t vertexHash, uint64_t fragmentHash, std::string_view vertex, std::string_view fragment, std::string_view origin, bool cg = false);
const SceGxmProgram* runtime_shader_binary(uint64_t hash, RuntimeShaderStage stage);

// Only after all patched programs and the GXM device have been destroyed.
void clear_runtime_shaders() noexcept;
} // namespace aurora::gxm
