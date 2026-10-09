#pragma once
// Scene feedback shaders. Standard library only: the GXM shader bank
// generator compiles every copy mode and the glare brightness program
// (docs/optimizations/rendering/offline-shader-bank.md).
#include <string>
#include <string_view>

namespace aurora::gfx::scene_feedback::shaders {
// Copy modes: bit 1 four-sample box filter, bit 0 RGB565 quantization.
inline constexpr unsigned CopyModes = 4;
inline std::string copy_label(unsigned mode) {
  return "Native scene feedback capture " + std::to_string(mode);
}

inline constexpr std::string_view BrightLabel = "Native scene glare brightness";
inline constexpr std::string_view Vertex = R"(
out vec2 v_uv;
const vec2 positions[3] = vec2[3](vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));
const vec2 uvs[3] = vec2[3](vec2(0.0, 0.0), vec2(0.0, 2.0), vec2(2.0, 0.0));
void main() { gl_Position = aur_position(vec4(positions[gl_VertexID], 0.0, 1.0)); v_uv = uvs[gl_VertexID]; }
)";
inline constexpr std::string_view SampleVertex = R"(
uniform vec4 source_rect;
uniform vec4 options;
out vec2 v_sample0;
const vec2 positions[3] = vec2[3](vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));
const vec2 uvs[3] = vec2[3](vec2(0.0, 0.0), vec2(0.0, 2.0), vec2(2.0, 0.0));
void main() {
  gl_Position=aur_position(vec4(positions[gl_VertexID],0.0,1.0));
  v_sample0=source_rect.xy+uvs[gl_VertexID]*source_rect.zw;
}
)";
inline std::string copy_vertex(unsigned mode) {
  const bool box = mode & 2;
  std::string source(SampleVertex);

  if (!box)
    return source;

  source.insert(source.find("const vec2 positions"), "out vec2 v_sample1;\nout vec2 v_sample2;\nout vec2 v_sample3;\n");
  source.insert(source.rfind('}'),
                "  vec2 uv=v_sample0;\n  v_sample0=uv-options.xy;\n  v_sample1=uv+vec2(options.x,-options.y);\n  v_sample2=uv+vec2(-options.x,options.y);\n  v_sample3=uv+options.xy;\n");
  return source;
}

inline std::string copy_fragment(unsigned mode) {
  const bool box = mode & 2, rgb565 = mode & 1;
  std::string source = "uniform sampler2D tex0;\nin vec2 v_sample0;\n";

  if (box)
    source += "in vec2 v_sample1;\nin vec2 v_sample2;\nin vec2 v_sample3;\n";

  source += "layout(location = 0) out vec4 frag_color;\nvoid main() {\n  vec4 c=";
  source += box ? "0.25*(texture(tex0,v_sample0)+texture(tex0,v_sample1)+texture(tex0,v_sample2)+texture(tex0,v_sample3));\n" : "texture(tex0,v_sample0);\n";

  if (rgb565)
    source += "  c=vec4(floor(clamp(c.rgb,0.0,1.0)*vec3(31.0,63.0,31.0)+0.5)/vec3(31.0,63.0,31.0),1.0);\n";

  source += "  frag_color=c;\n}\n";
  return source;
}

inline constexpr std::string_view Bright = R"(
uniform sampler2D tex0;
uniform vec4 thresholds;
uniform vec4 scale;
in vec2 v_uv;
layout(location = 0) out vec4 frag_color;
void main() {
  vec3 rgb=texture(tex0,v_uv).rgb;
  float l=dot(rgb,vec3(77.0,150.0,29.0)/256.0);
  if(l<thresholds.x) l-=l*thresholds.z;
  if(l>thresholds.y) l+=l*thresholds.w;
  l=clamp(l,0.0,1.0)*scale.x;
  frag_color=vec4(l,l,l,1.0);
}
)";
} // namespace aurora::gfx::scene_feedback::shaders
