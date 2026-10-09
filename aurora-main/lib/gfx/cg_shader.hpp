#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace aurora::gfx::cg {
enum class Stage { Vertex, Fragment };
inline constexpr uint32_t Version = 4;

// Translate Aurora's generated GLSL dialect, not arbitrary GLSL. Unsupported
// interfaces/layouts throw std::runtime_error instead of guessing a GPU ABI.
// Normalize live alpha references before calling, when source hashes must match
// Vita's parameterized programs. No filesystem, compiler or graphics context.
std::string convert(std::string_view source, Stage stage);
// F32_R targets require a declared scalar output, not an RGBA program bound to R.
std::string scalar_fragment(std::string_view fragmentCg);
} // namespace aurora::gfx::cg
