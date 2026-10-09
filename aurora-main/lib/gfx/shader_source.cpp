#include "shader_source.hpp"

namespace aurora::gl {
namespace {
constexpr std::string_view Prelude = R"(precision highp float;
precision highp int;
precision highp sampler2D;
precision highp usampler2D;
precision highp isampler2D;
#define f32 float
#define i32 int
#define u32 uint
#define vec2f vec2
#define vec3f vec3
#define vec4f vec4
#define vec2i ivec2
#define vec3i ivec3
#define vec4i ivec4
#define vec2u uvec2
#define vec3u uvec3
#define vec4u uvec4
#define mat4x4f mat4
#define mat3x4f mat3x4
float select(float f, float t, bool c) { return c ? t : f; }
vec2 select(vec2 f, vec2 t, bool c) { return c ? t : f; }
vec3 select(vec3 f, vec3 t, bool c) { return c ? t : f; }
vec4 select(vec4 f, vec4 t, bool c) { return c ? t : f; }
int select(int f, int t, bool c) { return c ? t : f; }
uint select(uint f, uint t, bool c) { return c ? t : f; }
ivec2 select(ivec2 f, ivec2 t, bool c) { return c ? t : f; }
uvec2 select(uvec2 f, uvec2 t, bool c) { return c ? t : f; }
vec2 select(vec2 f, vec2 t, bvec2 c) { return mix(f, t, c); }
vec3 select(vec3 f, vec3 t, bvec3 c) { return mix(f, t, c); }
vec4 select(vec4 f, vec4 t, bvec4 c) { return mix(f, t, c); }
// Aurora works in WebGPU conventions: clip-space z in [0, w] and framebuffer
// rows counted from the top. Every pass renders vertically flipped, so GL
// window row 0 is the top row and gl_FragCoord matches WebGPU's position.
vec4 aur_position(vec4 p) { return vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w); }
// Read-only word buffers live in R32UI textures, 2048 words per row.
uint aur_word(highp usampler2D b, uint i) { return texelFetch(b, ivec2(int(i & 2047u), int(i >> 11u)), 0).r; }
)";

}

std::string_view glsl_prelude() noexcept {
  return Prelude;
}

std::string vertex_source(std::string_view body) {
  std::string out = "#version 300 es\n";
  out += Prelude;
  out += "#line 1\n";
  out += body;
  return out;
}

std::string fragment_source(std::string_view body) {
  std::string out = "#version 300 es\n";
  out += Prelude;
  out += "#line 1\n";
  out += body;
  return out;
}

} // namespace aurora::gl
