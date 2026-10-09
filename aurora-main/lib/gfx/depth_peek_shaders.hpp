#pragma once
#include <string_view>

namespace aurora::gfx::depth_peek::shaders {
inline constexpr std::string_view VertexShader = R"(
const vec2 positions[3] = vec2[3](vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));

void main() {
    gl_Position = aur_position(vec4(positions[gl_VertexID], 0.0, 1.0));
}
)";

inline constexpr std::string_view FragmentShader = R"(
uniform highp sampler2D src;
uniform vec4 params; // offset.xy, scale.xy
layout(location = 0) out vec4 frag_color;

uint gx_z24(float depth) {
    return min(uint(clamp(depth, 0.0, 1.0) * 16777215.0 + 0.5), 0x00ffffffu);
}

void main() {
    vec2 dstCenter = floor(gl_FragCoord.xy) + vec2(0.5);
    ivec2 srcSize = textureSize(src, 0);
    ivec2 srcPixel = clamp(ivec2(floor(params.xy + dstCenter * params.zw)), ivec2(0), srcSize - ivec2(1));
    float depth = texelFetch(src, srcPixel, 0).r;
    uint z = gx_z24(DEPTH_EXPR);
    frag_color = vec4(uvec4(z & 0xffu, (z >> 8u) & 0xffu, z >> 16u, 255u)) / 255.0;
}
)";
}
