#pragma once
// Every fixed GLSL program the shared renderer modules create, with the exact
// sources their gl::ProgramDesc receives. Standard library only: the GXM shader
// bank generator (scripts/vita_shader_bank.cpp) compiles each of these
// (docs/vita-shader-bank.md). A module that creates a new program adds it here.
#include <string>
#include <vector>

namespace aurora::gfx {
struct FixedShaderSource {
  std::string label;
  std::string vertex;   // gl::vertex_source(...), as passed to ProgramDesc
  std::string fragment; // gl::fragment_source(...), as passed to ProgramDesc
};

std::vector<FixedShaderSource> fixed_shader_sources();
} // namespace aurora::gfx
