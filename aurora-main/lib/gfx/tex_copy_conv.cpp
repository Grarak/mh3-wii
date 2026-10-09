#include "tex_copy_conv.hpp"
#include "tex_copy_conv_shaders.hpp"
#include "tex_copy_format_contract.hpp"

#include "../internal.hpp"
#include "../gx/gx.hpp"
#include "texture.hpp"
#include "../gx/gx_fmt.hpp"

#include <aurora/fast_map.h>

#include "texture_convert.hpp"

namespace aurora::gfx::tex_copy_conv {
static Module Log("aurora::gfx::tex_copy_conv");

static FastMap<GXTexFmt, gl::ProgramPtr> g_programs;
static gl::ProgramPtr g_blitProgram;

static gl::ProgramPtr create_program(const shaders::Conversion& conversion) {
  return std::make_shared<gl::Program>(gl::ProgramDesc{
      .label = conversion.label,
      .vertex = gl::vertex_source(shaders::VertexShader),
      .fragment = gl::fragment_source(shaders::fragment(conversion)),
      .samplers = {{"src", 0}},
      .blocks = {{"UVTransform", 0}},
  });
}

bool needs_conversion(const GXTexFmt fmt) {
  return g_programs.contains(fmt);
}

void initialize() {
  g_blitProgram = create_program(shaders::Blit);

  for (const auto& conversion : shaders::Conversions) {
    g_programs[conversion.fmt] = create_program(conversion);

    if (to_gl_format(conversion.fmt) != gl::Format::RGBA8) {
      Log.fatal("Output format mismatch for {}", conversion.fmt);
    }
  }
}

void shutdown() {
  g_programs.clear();
  g_blitProgram.reset();
}

static void execute(const ConvRequest& req, gl::Program& program) {
  const GLuint prog = program.get();

  if (prog == 0 || !req.src || !req.dst || !req.dst->texture) {
    return;
  }

  const auto& dst = *req.dst;
  gl::bind_framebuffer(dst.texture->gl(), 0);
  gl::set_scissor_enabled(false);
  gl::set_viewport(0, 0, static_cast<int32_t>(dst.size.width), static_cast<int32_t>(dst.size.height), 0.f, 1.f);
  gl::apply_raster_state({});
  const bool linear = req.sampleFilter == SampleFilter::Linear && !gx::is_depth_format(req.fmt);
  gl::bind_texture(0, req.src->gl(), gl::sampler(linear ? gl::LinearClamp : gl::NearestClamp));
  gl::bind_uniform_range(0, uniform_buffer(), req.uniformRange.offset, req.uniformRange.size);
  gl::draw_fullscreen(prog);
}

void run(const ConvRequest& req) {
  const auto it = g_programs.find(req.fmt);

  if (it == g_programs.end()) {
    Log.fatal("No copy conversion program for format {}", static_cast<int>(req.fmt));
  }

  execute(req, *it->second);
}

void blit(const ConvRequest& req) {
  execute(req, *g_blitProgram);
}

} // namespace aurora::gfx::tex_copy_conv
