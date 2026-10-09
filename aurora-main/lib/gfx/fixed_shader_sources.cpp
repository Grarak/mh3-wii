#include "fixed_shader_sources.hpp"
#include "depth_peek_shaders.hpp"
#include "generated_texture_shaders.hpp"
#include "native_scene_feedback_shaders.hpp"
#include "shader_source.hpp"
#include "tex_copy_conv_shaders.hpp"
#include "tex_palette_conv_shaders.hpp"

namespace aurora::gfx {
// Each entry mirrors its module's ProgramDesc construction.
std::vector<FixedShaderSource> fixed_shader_sources() {
  std::vector<FixedShaderSource> out;
  const auto add = [&](std::string label, std::string_view vertex, std::string_view fragment) {
    out.push_back({std::move(label), gl::vertex_source(vertex), gl::fragment_source(fragment)});
  };
  // depth_peek.cpp initialize()
  add("Depth Peek", depth_peek::shaders::VertexShader, depth_peek::shaders::fragment_body());
  // tex_copy_conv.cpp initialize()
  add(tex_copy_conv::shaders::Blit.label, tex_copy_conv::shaders::VertexShader, tex_copy_conv::shaders::fragment(tex_copy_conv::shaders::Blit));
  for (const auto& conversion : tex_copy_conv::shaders::Conversions)
    add(conversion.label, tex_copy_conv::shaders::VertexShader, tex_copy_conv::shaders::fragment(conversion));
  // tex_palette_conv.cpp initialize()
  for (const auto& program : tex_palette_conv::shaders::Programs)
    add(program.label, tex_palette_conv::shaders::VertexShader, program.fragment);
  // generated_texture.cpp publish_generated_material()
  add(std::string(generated_texture::shaders::Label), generated_texture::shaders::VertexShader, generated_texture::shaders::FragmentShader);
  // native_scene_feedback.cpp programs()
  for (unsigned mode = 0; mode < scene_feedback::shaders::CopyModes; ++mode)
    add(scene_feedback::shaders::copy_label(mode), scene_feedback::shaders::copy_vertex(mode), scene_feedback::shaders::copy_fragment(mode));
  add(std::string(scene_feedback::shaders::BrightLabel), scene_feedback::shaders::Vertex, scene_feedback::shaders::Bright);
  return out;
}
} // namespace aurora::gfx
