#pragma once
// GLSL ES program sources as every renderer program receives them. Standard
// library only: the GXM shader bank generator (runtime/tools) links it.
#include <string>
#include <string_view>

namespace aurora::gl {
// The shared GLSL ES prelude: version, precision, WGSL-style aliases and helpers.
std::string_view glsl_prelude() noexcept;
std::string vertex_source(std::string_view body);
std::string fragment_source(std::string_view body);
} // namespace aurora::gl
