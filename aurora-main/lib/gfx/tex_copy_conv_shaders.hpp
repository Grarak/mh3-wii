#pragma once
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
}
