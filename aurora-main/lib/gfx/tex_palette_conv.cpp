#include "tex_palette_conv.hpp"
#include "tex_palette_conv_shaders.hpp"

#include "../internal.hpp"
#include "texture.hpp"

namespace aurora::gfx::tex_palette_conv {
static Module Log("aurora::gfx::tex_palette_conv");

static gl::ProgramPtr g_direct;
static gl::ProgramPtr g_fromFloat8;
static gl::ProgramPtr g_fromFloat4;

static gl::ProgramPtr create_program(Variant variant) {
  const auto& program = shaders::Programs.at(size_t(variant));
  return std::make_shared<gl::Program>(gl::ProgramDesc{
      .label = program.label,
      .vertex = gl::vertex_source(shaders::VertexShader),
      .fragment = gl::fragment_source(program.fragment),
      .samplers = {{"src", 0}, {"tlut", 1}},
  });
}

static gl::Program& program_for_variant(Variant variant) {
  switch (variant) {
  case Variant::Direct:
    return *g_direct;
  case Variant::FromFloat8:
    return *g_fromFloat8;
  case Variant::FromFloat4:
    return *g_fromFloat4;
  }

  FATAL("invalid palette conv variant {}", static_cast<int>(variant));
}

void initialize() {
  g_direct = create_program(Variant::Direct);
  g_fromFloat8 = create_program(Variant::FromFloat8);
  g_fromFloat4 = create_program(Variant::FromFloat4);
}

void shutdown() {
  g_direct.reset();
  g_fromFloat8.reset();
  g_fromFloat4.reset();
}

void run(const ConvRequest& req) {
  const GLuint prog = program_for_variant(req.variant).get();

  if (prog == 0 || !req.src || !req.dst || !req.tlut) {
    return;
  }

  gl::bind_framebuffer(req.dst->texture->gl(), 0);
  gl::set_scissor_enabled(false);
  gl::set_viewport(0, 0, static_cast<int32_t>(req.dst->size.width), static_cast<int32_t>(req.dst->size.height), 0.f, 1.f);
  gl::apply_raster_state({});
  const GLuint nearest = gl::sampler(gl::NearestClamp);
  gl::bind_texture(0, req.src->texture->gl(), nearest);
  gl::bind_texture(1, req.tlut->texture->gl(), nearest);
  gl::draw_fullscreen(prog);
}

} // namespace aurora::gfx::tex_palette_conv
