#include "tex_copy_conv.hpp"
#include "tex_copy_conv_shaders.hpp"
#include "tex_copy_format_contract.hpp"

#include "../internal.hpp"
#include "../gx/gx.hpp"
#include "texture.hpp"
#include "../gx/gx_fmt.hpp"

#include <absl/container/flat_hash_map.h>

#include "texture_convert.hpp"

using namespace std::string_literals;

namespace aurora::gfx::tex_copy_conv {
static Module Log("aurora::gfx::tex_copy_conv");

// Fullscreen triangle; uv (0, 0) is the top-left texel like the WebGPU original.
using shaders::VertexShader;

using shaders::ShaderPreamble;

static const std::string DepthShaderPreamble = R"(
uniform highp sampler2D src;

layout(std140) uniform UVTransform {
    vec2 offset;
    vec2 scale;
    vec4 copy_filter;
    vec4 flags;
} uv_xf;

in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

uint gx_z24_at_coord(ivec2 unclamped_coord) {
    ivec2 tex_size = textureSize(src, 0);
    ivec2 coord = clamp(unclamped_coord, ivec2(0), tex_size - ivec2(1));
    float depth = texelFetch(src, coord, 0).r;
)"s + (gx::UseReversedZ ? "    depth = 1.0 - depth;\n"s : ""s) + R"(
    return min(uint(clamp(depth, 0.0, 1.0) * 16777215.0 + 0.5), 0x00ffffffu);
}

uvec3 gx_depth_bytes(uint z24) {
    return uvec3((z24 >> 16u) & 0xffu, (z24 >> 8u) & 0xffu, z24 & 0xffu);
}

ivec2 clamp_copy_coord(ivec2 coord, ivec2 tex_size) {
    int top = int(floor(uv_xf.flags.z * float(tex_size.y)));
    int bottom = int(floor(uv_xf.flags.w * float(tex_size.y)));
    return ivec2(coord.x, clamp(coord.y, top, bottom));
}

uvec4 sample_depth_copy(vec2 uv) {
    ivec2 tex_size = textureSize(src, 0);
    ivec2 current_coord = clamp_copy_coord(ivec2(floor(uv * vec2(tex_size))), tex_size);
    uvec3 current = gx_depth_bytes(gx_z24_at_coord(current_coord));
    if (uv_xf.copy_filter.w == 0.0) {
        return uvec4(current, 255u);
    }

    // GX applies its vertical copy filter to depth copies too, filtering the high/middle/low Z bytes
    // independently before the destination format picks bytes. Alpha is unfiltered and 255.
    int row_stride = max(int(round(uv_xf.flags.y)), 1);
    uvec3 prev = gx_depth_bytes(gx_z24_at_coord(clamp_copy_coord(current_coord - ivec2(0, row_stride), tex_size)));
    uvec3 next = gx_depth_bytes(gx_z24_at_coord(clamp_copy_coord(current_coord + ivec2(0, row_stride), tex_size)));
    uvec3 coefficients = uvec3(uv_xf.copy_filter.xyz);
    uvec3 combined = prev * coefficients.x + current * coefficients.y + next * coefficients.z;
    uvec3 filtered = combined >> uvec3(6u);

    // The copy-filter accumulator wraps to nine bits when coefficients can
    // produce values at or above 512, before saturating to an eight-bit byte.
    if (coefficients.x + coefficients.y + coefficients.z >= 128u) {
        filtered = filtered & uvec3(0x1ffu);
    }
    filtered = min(filtered, uvec3(255u));
    return uvec4(filtered, 255u);
}
)"s;

// Passthrough blit (for scaling)
using shaders::FragPassthrough;

// GX_TF_I4: 4-bit intensity (quantized)
static constexpr std::string_view FragI4 = R"(
void main() {
    float i = quantize4(intensity(sample_copy(v_uv).rgb));
    frag_color = vec4(i, i, i, i);
}
)"sv;

// GX_TF_I8: 8-bit intensity
static constexpr std::string_view FragI8 = R"(
void main() {
    float i = intensity(sample_copy(v_uv).rgb);
    frag_color = vec4(i, i, i, i);
}
)"sv;

// GX_TF_IA4: 4-bit intensity + 4-bit alpha
static constexpr std::string_view FragIA4 = R"(
void main() {
    vec4 c = sample_copy(v_uv);
    float i = quantize4(intensity(c.rgb));
    frag_color = vec4(i, i, i, quantize4(c.a));
}
)"sv;

// GX_TF_IA8: 8-bit intensity + 8-bit alpha
static constexpr std::string_view FragIA8 = R"(
void main() {
    vec4 c = sample_copy(v_uv);
    float i = intensity(c.rgb);
    frag_color = vec4(i, i, i, c.a);
}
)"sv;

// GX_TF_RGB565: Blit alpha to 1.0
static constexpr std::string_view FragRGB565 = R"(
void main() {
    frag_color = vec4(sample_copy(v_uv).rgb, 1.0);
}
)"sv;

// GX_CTF_R4: 4-bit red
static constexpr std::string_view FragR4 = R"(
void main() {
    float r = quantize4(sample_copy(v_uv).r);
    frag_color = vec4(r, r, r, r);
}
)"sv;

// GX_CTF_RA4: 4-bit red + 4-bit alpha
static constexpr std::string_view FragRA4 = R"(
void main() {
    vec4 c = sample_copy(v_uv);
    float r = quantize4(c.r);
    frag_color = vec4(r, r, r, quantize4(c.a));
}
)"sv;

// GX_CTF_RA8: 8-bit red + 8-bit alpha
static constexpr std::string_view FragRA8 = R"(
void main() {
    vec4 c = sample_copy(v_uv);
    frag_color = vec4(c.r, c.r, c.r, c.a);
}
)"sv;

// GX_CTF_A8: 8-bit alpha
static constexpr std::string_view FragA8 = R"(
void main() {
    float a = sample_copy(v_uv).a;
    frag_color = vec4(a, a, a, a);
}
)"sv;

// GX_CTF_R8: 8-bit red
static constexpr std::string_view FragR8 = R"(
void main() {
    float r = sample_copy(v_uv).r;
    frag_color = vec4(r, r, r, r);
}
)"sv;

// GX_CTF_G8: 8-bit green
static constexpr std::string_view FragG8 = R"(
void main() {
    float g = sample_copy(v_uv).g;
    frag_color = vec4(g, g, g, g);
}
)"sv;

// GX_CTF_B8: 8-bit blue
static constexpr std::string_view FragB8 = R"(
void main() {
    float b = sample_copy(v_uv).b;
    frag_color = vec4(b, b, b, b);
}
)"sv;

// GX_CTF_RG8: 8-bit red + 8-bit green
static constexpr std::string_view FragRG8 = R"(
void main() {
    vec4 c = sample_copy(v_uv);
    frag_color = vec4(c.r, c.r, c.r, c.g);
}
)"sv;

// GX_CTF_GB8: 8-bit green + 8-bit blue
static constexpr std::string_view FragGB8 = R"(
void main() {
    vec4 c = sample_copy(v_uv);
    frag_color = vec4(c.g, c.g, c.g, c.b);
}
)"sv;

// GX_TF_Z8 stores the high-Z byte in I8 storage, which samples as intensity
// replicated across all four channels.
static constexpr std::string_view FragZ8 = R"(
void main() {
    float z8 = float(sample_depth_copy(v_uv).r) / 255.0;
    frag_color = vec4(z8, z8, z8, z8);
}
)"sv;

// GX_TF_Z16 is the depth RA8 copy path: depth samples have opaque alpha and their high-Z byte in
// red, so the encoded [A, R] pair samples through IA8 as high-Z intensity with opaque alpha.
static constexpr std::string_view FragZ16 = detail::Z16FragmentShader;

// GX_TF_Z24X8 uses the RGBA8 storage shape for copies, with the 24-bit depth
// value available to z-texture sampling through RGB.
static constexpr std::string_view FragZ24X8 = R"(
void main() {
    uvec4 depth_bytes = sample_depth_copy(v_uv);
    frag_color = vec4(vec3(depth_bytes.rgb) / 255.0, 1.0);
}
)"sv;

struct ConvShader {
  GXTexFmt fmt;
  std::string_view fragShader;
  const char* label;
};

static constexpr std::array ConvShaders{
    ConvShader{GX_TF_I4, FragI4, "TexCopyConv I4"},       ConvShader{GX_TF_I8, FragI8, "TexCopyConv I8"},
    ConvShader{GX_TF_IA4, FragIA4, "TexCopyConv IA4"},    ConvShader{GX_TF_IA8, FragIA8, "TexCopyConv IA8"},
    ConvShader{GX_TF_RGB565, FragRGB565, "TexCopyConv RGB565"},
    ConvShader{GX_CTF_R4, FragR4, "TexCopyConv R4"},      ConvShader{GX_CTF_RA4, FragRA4, "TexCopyConv RA4"},
    ConvShader{GX_CTF_RA8, FragRA8, "TexCopyConv RA8"},   ConvShader{GX_CTF_A8, FragA8, "TexCopyConv A8"},
    ConvShader{GX_CTF_R8, FragR8, "TexCopyConv R8"},      ConvShader{GX_CTF_G8, FragG8, "TexCopyConv G8"},
    ConvShader{GX_CTF_B8, FragB8, "TexCopyConv B8"},      ConvShader{GX_CTF_RG8, FragRG8, "TexCopyConv RG8"},
    ConvShader{GX_CTF_GB8, FragGB8, "TexCopyConv GB8"},
};

static constexpr std::array DepthConvShaders{
    ConvShader{GX_TF_Z8, FragZ8, "TexCopyConv Z8"},
    ConvShader{GX_TF_Z16, FragZ16, "TexCopyConv Z16"},
    ConvShader{GX_TF_Z24X8, FragZ24X8, "TexCopyConv Z24X8"},
};

static absl::flat_hash_map<GXTexFmt, gl::ProgramPtr> g_programs;
static gl::ProgramPtr g_blitProgram;

static gl::ProgramPtr create_program(const ConvShader& conv, std::string_view preamble) {
  std::string fragment;
  fragment.reserve(preamble.size() + conv.fragShader.size());
  fragment += preamble;
  fragment += conv.fragShader;
  return std::make_shared<gl::Program>(gl::ProgramDesc{
      .label = conv.label,
      .vertex = gl::vertex_source(VertexShader),
      .fragment = gl::fragment_source(fragment),
      .samplers = {{"src", 0}},
      .blocks = {{"UVTransform", 0}},
  });
}

bool needs_conversion(const GXTexFmt fmt) { return g_programs.contains(fmt); }

void initialize() {
  g_blitProgram = create_program({GX_TF_RGBA8, FragPassthrough, "TexCopyConv Blit"}, ShaderPreamble);
  for (const auto& conv : ConvShaders) {
    g_programs[conv.fmt] = create_program(conv, ShaderPreamble);
    if (to_gl_format(conv.fmt) != gl::Format::RGBA8) {
      Log.fatal("Output format mismatch for {}", conv.fmt);
    }
  }
  for (const auto& conv : DepthConvShaders) {
    g_programs[conv.fmt] = create_program(conv, DepthShaderPreamble);
    if (to_gl_format(conv.fmt) != gl::Format::RGBA8) {
      Log.fatal("Output format mismatch for {}", conv.fmt);
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

void blit(const ConvRequest& req) { execute(req, *g_blitProgram); }

} // namespace aurora::gfx::tex_copy_conv
