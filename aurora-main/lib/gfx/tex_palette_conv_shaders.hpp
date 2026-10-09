#pragma once
// Palette conversion shaders. Standard library only: the GXM shader bank
// generator compiles every Programs entry (docs/vita-shader-bank.md).
#include <array>
#include <string_view>

namespace aurora::gfx::tex_palette_conv::shaders {
// Fullscreen triangle; uv (0, 0) is the top-left texel.
inline constexpr std::string_view VertexShader = R"(
out vec2 v_uv;

const vec2 positions[3] = vec2[3](vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));
const vec2 uvs[3] = vec2[3](vec2(0.0, 0.0), vec2(0.0, 2.0), vec2(2.0, 0.0));

void main() {
    gl_Position = aur_position(vec4(positions[gl_VertexID], 0.0, 1.0));
    v_uv = uvs[gl_VertexID];
}
)";

// Direct: R16I index texture + TLUT -> RGBA8
inline constexpr std::string_view ShaderDirect = R"(
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
)";

// FromFloat8: float texture -> 8-bit index -> TLUT -> RGBA8
inline constexpr std::string_view ShaderFromFloat8 = R"(
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
)";

// FromFloat4: float texture -> 4-bit index -> TLUT -> RGBA8
inline constexpr std::string_view ShaderFromFloat4 = R"(
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
)";

struct Program {
  std::string_view fragment;
  const char* label;
};
// Indexed by tex_palette_conv::Variant.
inline constexpr std::array Programs{
    Program{ShaderDirect, "TexPaletteConv Direct"},
    Program{ShaderFromFloat8, "TexPaletteConv FromFloat8"},
    Program{ShaderFromFloat4, "TexPaletteConv FromFloat4"},
};
} // namespace aurora::gfx::tex_palette_conv::shaders
