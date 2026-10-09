#pragma once
#include <psp2/gxm.h>
#include <cstdint>
#include <string_view>

namespace aurora::gxm {
enum class RuntimeShaderStage { Vertex, Fragment, ScalarFragment };
// Called on the graphics owner with the same normalized GLSL used by the bank.
// Embedded stages require neither source storage nor a compiler module.
void register_runtime_shader_sources(uint64_t vertexHash, uint64_t fragmentHash, std::string_view vertex, std::string_view fragment, std::string_view origin);
const SceGxmProgram* runtime_shader_binary(uint64_t hash, RuntimeShaderStage stage);
// Only after all patched programs and the GXM device have been destroyed.
void clear_runtime_shaders() noexcept;
} // namespace aurora::gxm
