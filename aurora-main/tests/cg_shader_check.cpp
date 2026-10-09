#include "gfx/cg_shader.hpp"
#include "gfx/port_capture.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using aurora::gfx::cg::Stage;
using aurora::gfx::cg::convert;
const std::string vertex = R"(#version 330 core
layout(location=0) in vec4 position_in;
layout(location=1) in vec2 uv_in;
layout(location=2) in vec4 color_in;
out vec2 native_uv;
out vec4 native_color;
void main() {
    gl_Position = vec4(position_in.x, -position_in.y, position_in.zw);
    native_uv = uv_in;
    native_color = color_in;
}
)";
const std::string fragment = R"(#version 330 core
#define u32 uint
in vec2 native_uv;
in vec4 native_color;
uniform sampler2D native_texture;
layout(location=0) out vec4 native_output;
void main() {
    uint alphaCompare = 123u;
    if (alphaCompare < 1u) discard;
    native_output = texture(native_texture, native_uv) * native_color;
}
)";
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void has(const std::string& source, const char* expected) {
  check(source.find(expected) != source.npos, expected);
}
template <class Function> void rejects(Function function) {
  try { function(); } catch (const std::runtime_error&) { return; }
  throw std::runtime_error("unsupported input was accepted");
}
std::string read(const char* path) {
  std::ifstream file(path); check(bool(file), "cannot read shader input");
  return {std::istreambuf_iterator<char>(file), {}};
}
void write(const char* path, const std::string& source) {
  std::ofstream file(path); file << source; check(bool(file), "cannot write shader output");
}
void unit_checks() {
  const auto v = convert(vertex, Stage::Vertex), f = convert(fragment, Stage::Fragment);
  has(v, "a_position_in : POSITION"); has(v, "a_uv_in : TEXCOORD0"); has(v, "a_color_in : COLOR0");
  has(v, "float4(position_in.x, position_in.y, position_in.zw)");
  has(v, "o_native_color : TEXCOORD1"); has(f, "i_native_color : TEXCOORD1");
  check(v.find("vwords[") == v.npos && f.find("awords[") == f.npos, "native shaders contain legacy word buffers");
  has(f, "return native_output;"); has(f, "sampler2D native_texture : TEXUNIT0;");
  const auto mesh = convert(R"(#version 330 core
layout(location=0) in vec4 mesh_position;
layout(location=2) in vec4 mesh_color0;
out vec4 color;
void main() { gl_Position=mesh_position; color=mesh_color0; }
)", Stage::Vertex);
  has(mesh, "a_mesh_color0 : COLOR0"); has(mesh, "o_color : TEXCOORD0");
  const auto indexed = convert(R"(#version 330 core
layout(location=0) in vec4 mesh_position;
layout(location=12) in vec4 mesh_matrix0;
layout(location=13) in vec4 mesh_matrix1;
layout(location=14) in vec4 mesh_matrix2;
layout(std140) uniform Ubuf {mat3x4 palette[20];} ubuf;
void main() {
    uint slot=uint(mesh_matrix0.x);
    gl_Position=vec4(mesh_position * ubuf.palette[slot], 1.0);
}
)", Stage::Vertex);
  has(indexed, "a_mesh_matrix0 : BLENDINDICES0");
  has(indexed, "a_mesh_matrix1 : BLENDINDICES1");
  has(indexed, "a_mesh_matrix2 : BLENDINDICES2");
  has(indexed, "mul(mesh_position, ubuf.palette[slot])");
  const auto legacy = convert(R"(uniform usampler2D vbuf;
uint aur_word(int b, uint i) { return 0u; }
void main(){gl_Position=vec4(uintBitsToFloat(aur_word(vbuf, uint(gl_VertexID))));}
)", Stage::Vertex);
  has(legacy, "uint vwords[786432] : BUFFER[1];"); has(legacy, "uint awords[2097152] : BUFFER[2];");
  has(legacy, "bit_cast<float>(aur_word(vbuf, uint(aur_vertex_id)))");
  const auto compact = convert("in vec4 color; out vec4 output_color; void main(){output_color=color;}", Stage::Fragment);
  has(compact, "return output_color;");
  const auto math = convert(R"(#version 330 core
struct Light { vec3 pos; vec3 dir; vec4 color; vec3 cos_att; vec3 dist_att; };
layout(std140) uniform Ubuf {
    vec3 position; float weight; vec2 uv; vec4 tint;
    Light lights[2]; mat4 transform[2];
} ubuf;
vec4 transformed(vec4 p) { return (p) * ubuf.transform[0]; }
void main() {
    gl_Position = vec4(1.0) - transformed(vec4(2.0)) * ubuf.transform[int(max(0, int(1)))];
    gl_Position += 1e-3 * vec4(1.0) * ubuf.transform[0];
}
)", Stage::Vertex);
  has(math, "float3 position;\n    float weight;\n    float2 uv;\n    float aur_padding_0;\n    float aur_padding_1;\n    float4 tint;");
  has(math, "float3 pos;\n    float aur_padding_0;\n    float3 dir;");
  has(math, "return mul((p), ubuf.transform[0]);");
  has(math, "float4(1.0) - mul(transformed(float4(2.0)), ubuf.transform[int(max(0, int(1)))]);");
  has(math, "mul(1e-3 * float4(1.0), ubuf.transform[0]);");
  const auto nested = convert(R"(uniform sampler2D src;
out vec4 color;
void main() {
    // texture(src, deliberately unfinished comment
    color = texture(src, texture(src, vec2(0.5)).xy);
}
)", Stage::Fragment);
  has(nested, "tex2D(src, tex2D(src, float2(0.5)).xy)");
  const auto scalar = aurora::gfx::cg::scalar_fragment(
    "float4 helper(){return float4(1.0);}\nfloat4 main(float4 x : WPOS) : COLOR0 {return helper();}\n");
  has(scalar, "helper(){return float4(1.0);}"); has(scalar, "__regformat float main"); has(scalar, "return (helper()).r;");
  rejects([] { convert("layout(std140) uniform U {vec2 v[2];} ubuf; void main(){gl_Position=vec4(0.0);}", Stage::Vertex); });
  rejects([] { convert("layout(std140) uniform U {mat3 v;} ubuf; void main(){gl_Position=vec4(0.0);}", Stage::Vertex); });
  rejects([] { convert("layout(location=1) in vec4 position_in; void main(){gl_Position=position_in;}", Stage::Vertex); });
  rejects([] { convert("layout(location=0) in vec4 position_in; void main(){gl_Position=position_in+vec4(float(gl_VertexID));}", Stage::Vertex); });
  rejects([] { convert("layout(location=0) in vec4 mesh_position; layout(location=3) in vec4 native_color0; void main(){gl_Position=mesh_position;}", Stage::Vertex); });
  rejects([] { convert("layout(location=1) out vec4 color; void main(){color=vec4(1.0);}", Stage::Fragment); });
  rejects([] { convert("out vec4 a; out vec4 b; void main(){a=b=vec4(1.0);}", Stage::Fragment); });
  rejects([] { convert("flat in vec4 a; out vec4 b; void main(){b=a;}", Stage::Fragment); });
  rejects([] { convert("uniform sampler2D src; out vec4 color; void main(){color=texture(src,vec2(0.0);}", Stage::Fragment); });
  rejects([] { convert("uniform sampler2D src; out vec4 color; void main(){color=texelFetch(src,ivec2(0),1);}", Stage::Fragment); });
}
void capture_checks(const char* path) {
  check(setenv("AURORA_PORT_CAPTURE", path, 1) == 0, "cannot set capture directory");
  using aurora::gfx::port_capture::shader;
  shader("direct \"geometry\"", vertex, fragment, "test/generator.cpp:42 model=7");
  std::filesystem::path first;
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(path) / "shaders"))
    if (entry.path().extension() == ".cg") { first = entry.path(); break; }
  check(!first.empty(), "capture produced no Cg");
  const auto timestamp = std::filesystem::last_write_time(first);
  for (unsigned i = 0; i < 100; ++i)
    shader("direct \"geometry\"", vertex, fragment, "test/generator.cpp:42 model=7");
  check(std::filesystem::last_write_time(first) == timestamp, "repeated shader rewrote Cg");
  shader("second origin", vertex, fragment, "test/other.cpp:8");
  auto second = fragment; second.replace(second.find("< 1u"), 4, "< 2u");
  shader("alpha two", vertex, second, "test/generator.cpp:42 model=8");
  const std::string unsupported = "layout(location=1) out vec4 color; void main(){color=vec4(1.0);}";
  shader("unsupported", vertex, unsupported, "test/unsupported");
  shader("unsupported", vertex, unsupported, "test/unsupported");
}
} // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 5 && std::string(argv[1]) == "--convert") {
      const std::string stage(argv[2]); check(stage == "vertex" || stage == "fragment", "unknown shader stage");
      write(argv[4], convert(read(argv[3]), stage == "vertex" ? Stage::Vertex : Stage::Fragment));
    } else if (argc == 4 && std::string(argv[1]) == "--scalar") {
      write(argv[3], aurora::gfx::cg::scalar_fragment(read(argv[2])));
    } else if (argc == 3 && std::string(argv[1]) == "--capture") capture_checks(argv[2]);
    else { check(argc == 1, "invalid test arguments"); unit_checks(); std::cout << "Portable Cg checks passed\n"; }
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
