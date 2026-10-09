#pragma once
// Character material composition shaders. Standard library only: the GXM
// shader bank generator compiles this program (docs/optimizations/rendering/offline-shader-bank.md).
#include <string_view>

namespace aurora::gfx::generated_texture::shaders {
inline constexpr std::string_view Label = "Character material composition";
inline constexpr std::string_view VertexShader = R"(
out vec2 v_uv;
out vec2 v_atlas_uv;
uniform vec4 atlasOffset;
const vec2 positions[3] = vec2[3](vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));
const vec2 uvs[3] = vec2[3](vec2(0.0, 0.0), vec2(0.0, 2.0), vec2(2.0, 0.0));
void main() {
  gl_Position = aur_position(vec4(positions[gl_VertexID], 0.0, 1.0));
  v_uv = uvs[gl_VertexID];
  v_atlas_uv = v_uv + atlasOffset.xy;
}
)";
inline constexpr std::string_view FragmentShader = R"(
uniform sampler2D tex0;
uniform sampler2D tex1;
uniform sampler2D tex2;
uniform sampler2D tex3;
uniform vec4 params;
in vec2 v_uv;
in vec2 v_atlas_uv;
layout(location = 0) out vec4 frag_color;
void main() {
  vec4 base = texture(tex0, v_uv);
  vec4 color = texture(tex1, v_uv);
  vec4 detail = texture(tex2, v_uv);
  vec4 atlas = texture(tex3, v_atlas_uv);
  float alpha = color.a * params.x;
  vec3 result = mix(base.rgb, color.rgb, alpha);
  result = mix(result, detail.rgb, detail.a * params.y);
  float atlasWeight = step(0.75, v_uv.x) * step(0.875, v_uv.y) * params.z;
  frag_color = vec4(mix(result, atlas.rgb, atlasWeight), 1.0);
}
)";
} // namespace aurora::gfx::generated_texture::shaders
