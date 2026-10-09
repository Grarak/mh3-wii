#include "tex_palette_conv.hpp"

#include "../internal.hpp"
#include "texture.hpp"

namespace aurora::gfx::tex_palette_conv {
static Module Log("aurora::gfx::tex_palette_conv");

// Fullscreen triangle; uv (0, 0) is the top-left texel.
static constexpr std::string_view VertexShader = R"(
out vec2 v_uv;

const vec2 positions[3] = vec2[3](vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));
const vec2 uvs[3] = vec2[3](vec2(0.0, 0.0), vec2(0.0, 2.0), vec2(2.0, 0.0));

void main() {
    gl_Position = aur_position(vec4(positions[gl_VertexID], 0.0, 1.0));
    v_uv = uvs[gl_VertexID];
}
)"sv;

// Direct: R16I index texture + TLUT -> RGBA8
static constexpr std::string_view ShaderDirect = R"(
uniform highp isampler2D src;
uniform highp sampler2D tlut;
in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

void main() {
    vec2 texSize = vec2(textureSize(src, 0));
    ivec2 coord = ivec2(floor(v_uv * texSize));
    int idx = texelFetch(src, coord, 0).r;
    frag_color = texelFetch(tlut, ivec2(idx, 0), 0);
}
)"sv;

// FromFloat8: float texture -> 8-bit index -> TLUT -> RGBA8
static constexpr std::string_view ShaderFromFloat8 = R"(
uniform highp sampler2D src;
uniform highp sampler2D tlut;
in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

void main() {
    vec2 texSize = vec2(textureSize(src, 0));
    ivec2 coord = ivec2(floor(v_uv * texSize));
    float r = texelFetch(src, coord, 0).r;
    frag_color = texelFetch(tlut, ivec2(int(r * 255.0), 0), 0);
}
)"sv;

// FromFloat4: float texture -> 4-bit index -> TLUT -> RGBA8
static constexpr std::string_view ShaderFromFloat4 = R"(
uniform highp sampler2D src;
uniform highp sampler2D tlut;
in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

void main() {
    vec2 texSize = vec2(textureSize(src, 0));
    ivec2 coord = ivec2(floor(v_uv * texSize));
    float r = texelFetch(src, coord, 0).r;
    frag_color = texelFetch(tlut, ivec2(int(r * 15.0), 0), 0);
}
)"sv;

static gl::ProgramPtr g_direct;
static gl::ProgramPtr g_fromFloat8;
static gl::ProgramPtr g_fromFloat4;

static gl::ProgramPtr create_program(std::string_view fragment, const char* label) {
  return std::make_shared<gl::Program>(gl::ProgramDesc{
      .label = label,
      .vertex = gl::vertex_source(VertexShader),
      .fragment = gl::fragment_source(fragment),
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
  g_direct = create_program(ShaderDirect, "TexPaletteConv Direct");
  g_fromFloat8 = create_program(ShaderFromFloat8, "TexPaletteConv FromFloat8");
  g_fromFloat4 = create_program(ShaderFromFloat4, "TexPaletteConv FromFloat4");
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
