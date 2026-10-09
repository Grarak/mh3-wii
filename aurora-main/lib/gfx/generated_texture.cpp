#include "aurora/generated_texture.hpp"
#include "texture.hpp"
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace aurora {
namespace {
struct GeneratedTexture {
  uint32_t owner, version;
  gfx::TextureHandle handle;
  std::array<gfx::TextureHandle, 4> inputs;
};
std::unordered_map<const void*, GeneratedTexture> generatedTextures;
uint32_t generatedVersion = 0;
gl::ProgramPtr materialProgram;
constexpr const char* vertex = R"(
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
constexpr const char* fragment = R"(
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
  float alpha = floor(color.a * params.x * 255.0 + 0.5) / 255.0;
  vec3 result = floor(mix(base.rgb, color.rgb, alpha) * 255.0 + 0.5) / 255.0;
  result = floor(mix(result, detail.rgb, detail.a * params.y) * 255.0 + 0.5) / 255.0;
  float atlasWeight = step(0.75, v_uv.x) * step(0.875, v_uv.y) * params.z;
  frag_color = vec4(mix(result, atlas.rgb, atlasWeight), 1.0);
}
)";
gfx::TextureHandle source_texture(const MaterialSource& source) {
#ifdef AURORA_GXM
  if (source.nativeAsset) {
    const auto info = gl::native_texture_info(source.nativeAsset);
    if (info.width != 128 || info.height != 128) throw std::runtime_error("Native material source dimensions differ");
    return std::make_shared<gfx::TextureRef>(gl::native_texture_asset(source.nativeAsset),
        gfx::Extent{128, 128, 1}, info.format, info.mips, GX_TF_RGBA8_PC);
  }
#endif
  return gfx::new_static_texture_2d(128, 128, 1, source.format, source.bytes, false, "Material source");
}
}
void publish_generated_material(uint32_t owner, const void* guestData, MaterialComposition material) {
  if (!materialProgram) materialProgram = std::make_shared<gl::Program>(gl::ProgramDesc{
      .label = "Character material composition", .vertex = gl::vertex_source(vertex),
      .fragment = gl::fragment_source(fragment),
      .samplers = {{"tex0", 0}, {"tex1", 1}, {"tex2", 2}, {"tex3", 3}}});
  std::array<gfx::TextureHandle, 4> inputs;
  inputs[0] = source_texture(material.sources[0]);
  inputs[1] = source_texture(material.sources[1]);
  inputs[2] = material.optionalOverlay ? source_texture(material.sources[2]) : inputs[0];
  inputs[3] = material.atlas ? source_texture(material.sources[3]) : inputs[0];
  auto output = gfx::new_render_texture(128, 128, GX_TF_RGBA8_PC, "Generated character material");
  gl::post([output, inputs, program = materialProgram, opacity = material.opacity,
            detail = material.optionalOverlay, atlas = material.atlas, x = material.atlasX, y = material.atlasY] {
    const auto id = program->get();
    gl::bind_framebuffer(output->texture->gl(), 0);
    gl::set_viewport(0, 0, 128, 128, 0.f, 1.f);
    gl::set_scissor_enabled(false);
    gl::apply_raster_state({});
    gl::uniform4(id, "params", opacity, detail ? 1.f : 0.f, atlas ? 1.f : 0.f, 0.f);
    gl::uniform4(id, "atlasOffset", (float(x) - 96.f) / 128.f, (float(y) - 112.f) / 128.f, 0.f, 0.f);
    const auto sampler = gl::sampler(gl::NearestClamp);
    for (unsigned i = 0; i < inputs.size(); ++i) gl::bind_texture(i, inputs[i]->texture->gl(), sampler);
    gl::draw_fullscreen(id);
  });
  for (auto it = generatedTextures.begin(); it != generatedTextures.end();) {
    if (it->second.owner == owner) it = generatedTextures.erase(it);
    else ++it;
  }
  if (++generatedVersion == 0) throw std::runtime_error("Generated texture revision overflow");
  generatedTextures.insert_or_assign(guestData, GeneratedTexture{owner, generatedVersion, std::move(output), std::move(inputs)});
}
uint32_t generated_texture_version(const void* guestData, uint32_t width, uint32_t height) {
  if (width != 128 || height != 128) return 0;
  const auto it = generatedTextures.find(guestData);
  return it == generatedTextures.end() ? 0 : it->second.version;
}
void clear_generated_textures() { generatedTextures.clear(); materialProgram.reset(); }
namespace gfx {
TextureHandle find_generated_texture(const void* guestData, uint32_t version) {
  const auto it = generatedTextures.find(guestData);
  if (it == generatedTextures.end() || it->second.version != version)
    throw std::runtime_error("Generated texture was not published in GX command order");
  return it->second.handle;
}
}
}
