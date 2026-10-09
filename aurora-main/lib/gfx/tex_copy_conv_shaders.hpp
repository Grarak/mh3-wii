#pragma once
// Copy conversion shaders. Standard library and GX enums only: the GXM shader
// bank generator compiles Blit and every Conversions entry (docs/vita-shader-bank.md).
#include "tex_copy_format_contract.hpp"
#include "../gx/reversed_z.hpp"
#include <dolphin/gx/GXEnum.h>
#include <array>
#include <string>
#include <string_view>

namespace aurora::gfx::tex_copy_conv::shaders {
inline constexpr std::string_view VertexShader = R"(
layout(std140) uniform UVTransform {
    vec2 offset;
    vec2 scale;
    vec4 copy_filter;
    vec4 flags;
} uv_xf;

out vec2 v_uv;

const vec2 positions[3] = vec2[3](vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));
const vec2 uvs[3] = vec2[3](vec2(0.0, 0.0), vec2(0.0, 2.0), vec2(2.0, 0.0));

void main() {
    gl_Position = aur_position(vec4(positions[gl_VertexID], 0.0, 1.0));
    v_uv = uvs[gl_VertexID] * uv_xf.scale + uv_xf.offset;
}
)";

inline constexpr std::string_view ShaderPreamble = R"(
uniform highp sampler2D src;

layout(std140) uniform UVTransform {
    vec2 offset;
    vec2 scale;
    vec4 copy_filter;
    vec4 flags;
} uv_xf;

in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

float intensity(vec3 rgb) {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

float quantize4(float v) {
    return floor(v * 16.0) / 15.0;
}

vec4 apply_opaque_alpha(vec4 c) {
    if (uv_xf.flags.x != 0.0) {
        return vec4(c.rgb, 1.0);
    }
    return c;
}

vec2 clamp_copy_uv(vec2 uv) {
    return vec2(uv.x, clamp(uv.y, uv_xf.flags.z, uv_xf.flags.w));
}

vec4 sample_copy(vec2 uv) {
    vec4 current = texture(src, clamp_copy_uv(uv));
    if (uv_xf.copy_filter.w == 0.0) {
        return apply_opaque_alpha(current);
    }

    vec2 tex_size = vec2(textureSize(src, 0));
    vec2 pixel_size = vec2(1.0, 1.0) / tex_size;
    float row_stride = max(uv_xf.flags.y, 1.0);
    vec4 prev = texture(src, clamp_copy_uv(uv - vec2(0.0, pixel_size.y * row_stride)));
    vec4 next = texture(src, clamp_copy_uv(uv + vec2(0.0, pixel_size.y * row_stride)));
    vec3 prev_rgb = floor(prev.rgb * 255.0 + vec3(0.5));
    vec3 current_rgb = floor(current.rgb * 255.0 + vec3(0.5));
    vec3 next_rgb = floor(next.rgb * 255.0 + vec3(0.5));
    vec3 filtered_rgb = min(
        floor((prev_rgb * uv_xf.copy_filter.x +
               current_rgb * uv_xf.copy_filter.y +
               next_rgb * uv_xf.copy_filter.z) / 64.0),
        vec3(255.0));
    vec4 filtered = vec4(filtered_rgb / 255.0, current.a);
    return apply_opaque_alpha(filtered);
}
)";

inline constexpr std::string_view FragPassthrough = R"(
void main() {
    frag_color = sample_copy(v_uv);
}
)";

inline std::string depth_preamble() {
  using namespace std::string_literals;
  return R"(
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
)"s + (gx::UseReversedZ ? "    depth = 1.0 - depth;\n"s : ""s) +
                                               R"(
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
}

// GX_TF_I4: 4-bit intensity (quantized)
inline constexpr std::string_view FragI4 = R"(
void main() {
    float i = quantize4(intensity(sample_copy(v_uv).rgb));
    frag_color = vec4(i, i, i, i);
}
)";

// GX_TF_I8: 8-bit intensity
inline constexpr std::string_view FragI8 = R"(
void main() {
    float i = intensity(sample_copy(v_uv).rgb);
    frag_color = vec4(i, i, i, i);
}
)";

// GX_TF_IA4: 4-bit intensity + 4-bit alpha
inline constexpr std::string_view FragIA4 = R"(
void main() {
    vec4 c = sample_copy(v_uv);
    float i = quantize4(intensity(c.rgb));
    frag_color = vec4(i, i, i, quantize4(c.a));
}
)";

// GX_TF_IA8: 8-bit intensity + 8-bit alpha
inline constexpr std::string_view FragIA8 = R"(
void main() {
    vec4 c = sample_copy(v_uv);
    float i = intensity(c.rgb);
    frag_color = vec4(i, i, i, c.a);
}
)";

// GX_TF_RGB565: Blit alpha to 1.0
inline constexpr std::string_view FragRGB565 = R"(
void main() {
    frag_color = vec4(sample_copy(v_uv).rgb, 1.0);
}
)";

// GX_CTF_R4: 4-bit red
inline constexpr std::string_view FragR4 = R"(
void main() {
    float r = quantize4(sample_copy(v_uv).r);
    frag_color = vec4(r, r, r, r);
}
)";

// GX_CTF_RA4: 4-bit red + 4-bit alpha
inline constexpr std::string_view FragRA4 = R"(
void main() {
    vec4 c = sample_copy(v_uv);
    float r = quantize4(c.r);
    frag_color = vec4(r, r, r, quantize4(c.a));
}
)";

// GX_CTF_RA8: 8-bit red + 8-bit alpha
inline constexpr std::string_view FragRA8 = R"(
void main() {
    vec4 c = sample_copy(v_uv);
    frag_color = vec4(c.r, c.r, c.r, c.a);
}
)";

// GX_CTF_A8: 8-bit alpha
inline constexpr std::string_view FragA8 = R"(
void main() {
    float a = sample_copy(v_uv).a;
    frag_color = vec4(a, a, a, a);
}
)";

// GX_CTF_R8: 8-bit red
inline constexpr std::string_view FragR8 = R"(
void main() {
    float r = sample_copy(v_uv).r;
    frag_color = vec4(r, r, r, r);
}
)";

// GX_CTF_G8: 8-bit green
inline constexpr std::string_view FragG8 = R"(
void main() {
    float g = sample_copy(v_uv).g;
    frag_color = vec4(g, g, g, g);
}
)";

// GX_CTF_B8: 8-bit blue
inline constexpr std::string_view FragB8 = R"(
void main() {
    float b = sample_copy(v_uv).b;
    frag_color = vec4(b, b, b, b);
}
)";

// GX_CTF_RG8: 8-bit red + 8-bit green
inline constexpr std::string_view FragRG8 = R"(
void main() {
    vec4 c = sample_copy(v_uv);
    frag_color = vec4(c.r, c.r, c.r, c.g);
}
)";

// GX_CTF_GB8: 8-bit green + 8-bit blue
inline constexpr std::string_view FragGB8 = R"(
void main() {
    vec4 c = sample_copy(v_uv);
    frag_color = vec4(c.g, c.g, c.g, c.b);
}
)";

// GX_TF_Z8 stores the high-Z byte in I8 storage, which samples as intensity
// replicated across all four channels.
inline constexpr std::string_view FragZ8 = R"(
void main() {
    float z8 = float(sample_depth_copy(v_uv).r) / 255.0;
    frag_color = vec4(z8, z8, z8, z8);
}
)";

// GX_TF_Z16 is the depth RA8 copy path: depth samples have opaque alpha and their high-Z byte in
// red, so the encoded [A, R] pair samples through IA8 as high-Z intensity with opaque alpha.
inline constexpr std::string_view FragZ16 = detail::Z16FragmentShader;

// GX_TF_Z24X8 uses the RGBA8 storage shape for copies, with the 24-bit depth
// value available to z-texture sampling through RGB.
inline constexpr std::string_view FragZ24X8 = R"(
void main() {
    uvec4 depth_bytes = sample_depth_copy(v_uv);
    frag_color = vec4(vec3(depth_bytes.rgb) / 255.0, 1.0);
}
)";

struct Conversion {
  GXTexFmt fmt;
  std::string_view fragment;
  const char* label;
  bool depth;
};
// Passthrough blit (for scaling); its format is only a placeholder.
inline constexpr Conversion Blit{GX_TF_RGBA8, FragPassthrough, "TexCopyConv Blit", false};
inline constexpr std::array Conversions{
    Conversion{GX_TF_I4, FragI4, "TexCopyConv I4", false},
    Conversion{GX_TF_I8, FragI8, "TexCopyConv I8", false},
    Conversion{GX_TF_IA4, FragIA4, "TexCopyConv IA4", false},
    Conversion{GX_TF_IA8, FragIA8, "TexCopyConv IA8", false},
    Conversion{GX_TF_RGB565, FragRGB565, "TexCopyConv RGB565", false},
    Conversion{GX_CTF_R4, FragR4, "TexCopyConv R4", false},
    Conversion{GX_CTF_RA4, FragRA4, "TexCopyConv RA4", false},
    Conversion{GX_CTF_RA8, FragRA8, "TexCopyConv RA8", false},
    Conversion{GX_CTF_A8, FragA8, "TexCopyConv A8", false},
    Conversion{GX_CTF_R8, FragR8, "TexCopyConv R8", false},
    Conversion{GX_CTF_G8, FragG8, "TexCopyConv G8", false},
    Conversion{GX_CTF_B8, FragB8, "TexCopyConv B8", false},
    Conversion{GX_CTF_RG8, FragRG8, "TexCopyConv RG8", false},
    Conversion{GX_CTF_GB8, FragGB8, "TexCopyConv GB8", false},
    Conversion{GX_TF_Z8, FragZ8, "TexCopyConv Z8", true},
    Conversion{GX_TF_Z16, FragZ16, "TexCopyConv Z16", true},
    Conversion{GX_TF_Z24X8, FragZ24X8, "TexCopyConv Z24X8", true},
};
// The fragment body (preamble and main) of one conversion.
inline std::string fragment(const Conversion& conversion) {
  return (conversion.depth ? depth_preamble() : std::string(ShaderPreamble)) + std::string(conversion.fragment);
}
} // namespace aurora::gfx::tex_copy_conv::shaders
