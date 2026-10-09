#include <aurora/native_scene_feedback.hpp>
#include <aurora/aurora.h>
#include "../gl/device.hpp"
#include "../gl/gl.hpp"
#include "../dolphin/vi/vi_internal.hpp"
#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace aurora {
namespace {
struct Image {gl::TexturePtr texture;uint64_t revision;};
std::map<const void*,Image> images;
// Keep published textures alive while a native unit names their revision.
// Replacing a producer identity must not invalidate the previous binding.
std::array<gl::TexturePtr,16> sampledImages;
uint64_t revision=0;
std::array<gl::ProgramPtr,4> copyPrograms;
gl::ProgramPtr brightProgram;
constexpr const char* vertex=R"(
out vec2 v_uv;
const vec2 positions[3] = vec2[3](vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));
const vec2 uvs[3] = vec2[3](vec2(0.0, 0.0), vec2(0.0, 2.0), vec2(2.0, 0.0));
void main() { gl_Position = aur_position(vec4(positions[gl_VertexID], 0.0, 1.0)); v_uv = uvs[gl_VertexID]; }
)";
constexpr const char* sampleVertex=R"(
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
std::string copy_vertex(bool box){
  std::string source=sampleVertex;if(!box)return source;
  source.insert(source.find("const vec2 positions"),"out vec2 v_sample1;\nout vec2 v_sample2;\nout vec2 v_sample3;\n");
  source.insert(source.rfind('}'),"  vec2 uv=v_sample0;\n  v_sample0=uv-options.xy;\n  v_sample1=uv+vec2(options.x,-options.y);\n  v_sample2=uv+vec2(-options.x,options.y);\n  v_sample3=uv+options.xy;\n");return source;
}
std::string copy_fragment(bool box,bool rgb565){
  std::string source="uniform sampler2D tex0;\nin vec2 v_sample0;\n";
  if(box)source+="in vec2 v_sample1;\nin vec2 v_sample2;\nin vec2 v_sample3;\n";
  source+="layout(location = 0) out vec4 frag_color;\nvoid main() {\n  vec4 c=";
  source+=box?"0.25*(texture(tex0,v_sample0)+texture(tex0,v_sample1)+texture(tex0,v_sample2)+texture(tex0,v_sample3));\n":"texture(tex0,v_sample0);\n";
  if(rgb565)source+="  c=vec4(floor(clamp(c.rgb,0.0,1.0)*vec3(31.0,63.0,31.0)+0.5)/vec3(31.0,63.0,31.0),1.0);\n";
  source+="  frag_color=c;\n}\n";return source;
}
constexpr const char* bright=R"(
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
void owner(){if(!gl::on_render_thread())throw std::runtime_error("Scene feedback changed outside graphics owner");}
void dimensions(uint32_t w,uint32_t h){if(!w||!h||w>uint32_t(gl::caps().maxTextureSize)||h>uint32_t(gl::caps().maxTextureSize))throw std::runtime_error("Scene feedback target exceeds renderer dimensions");}
gl::TexturePtr target(uint32_t w,uint32_t h){dimensions(w,h);return gl::create_texture(w,h,1,gl::Format::RGBA8,"Native scene feedback");}
void programs(){
  for(unsigned mode=0;mode<4;mode++)if(!copyPrograms[mode])copyPrograms[mode]=std::make_shared<gl::Program>(gl::ProgramDesc{.label="Native scene feedback capture "+std::to_string(mode),.vertex=gl::vertex_source(copy_vertex(mode&2)),.fragment=gl::fragment_source(copy_fragment(mode&2,mode&1)),.samplers={{"tex0",0}}});
  if(!brightProgram)brightProgram=std::make_shared<gl::Program>(gl::ProgramDesc{.label="Native scene glare brightness",.vertex=gl::vertex_source(vertex),.fragment=gl::fragment_source(bright),.samplers={{"tex0",0}}});
}
struct Restore {gl::OffscreenState state=gl::save_offscreen_state();~Restore(){gl::restore_offscreen_state(state);}};
void publish(const void* id,gl::TexturePtr texture){if(!id)throw std::runtime_error("Scene feedback has no producer identity");if(++revision==0)throw std::runtime_error("Scene feedback revision overflow");images.insert_or_assign(id,Image{std::move(texture),revision});}
void copy_target(const gl::TexturePtr& source,const gl::TexturePtr& destination,
                  const std::array<float,4>& rect,bool box,bool rgb565){
  gl::bind_framebuffer(destination->gl(),0);gl::set_viewport(0,0,destination->width,destination->height,0,1);gl::set_scissor_enabled(false);gl::apply_raster_state({});
  auto program=copyPrograms[(box?2:0)|(rgb565?1:0)]->get();gl::uniform4(program,"source_rect",rect[0],rect[1],rect[2],rect[3]);
  if(box)gl::uniform4(program,"options",0.5f/float(source->width),0.5f/float(source->height),0,0);
  gl::bind_texture(0,source->gl(),gl::sampler(gl::LinearClamp));gl::draw_fullscreen(program);
}
}
bool publish_scene_capture(const void* id,const SceneCaptureParameters& p){
  owner();if(!aurora_native_frame_should_render())return false;
  if(p.format!=4&&p.format!=6)throw std::runtime_error("Unsupported scene feedback color format");
  const auto source=gpu::g_frameBuffer.texture;if(!source)throw std::runtime_error("Scene feedback has no current color target");
  for(float value:p.source)if(!std::isfinite(value))throw std::runtime_error("Nonfinite scene capture rectangle");
  if(p.source[0]<0||p.source[1]<0||p.source[2]<=0||p.source[3]<=0)throw std::runtime_error("Invalid scene capture rectangle");
  const auto config=vi::configured_fb_size();const float logicalW=float(config.x),logicalH=float(config.y);
  if(logicalW<=0||logicalH<=0||p.source[0]+p.source[2]>logicalW||p.source[1]+p.source[3]>logicalH)throw std::runtime_error("Scene capture rectangle exceeds logical color target");
  Restore restore;programs();auto output=target(p.width,p.height);const std::array<float,4> rect={p.source[0]/logicalW,p.source[1]/logicalH,p.source[2]/logicalW,p.source[3]/logicalH};
  copy_target(source,output,rect,p.box_filter,p.format==4);publish(id,std::move(output));return true;
}
bool publish_scene_glare(const void* id,const SceneGlareParameters& p){
  owner();if(!aurora_native_frame_should_render())return false;dimensions(p.width,p.height);
  if(!std::isfinite(p.low_threshold)||!std::isfinite(p.high_threshold)||!std::isfinite(p.darken)||!std::isfinite(p.brighten)||!std::isfinite(p.shift_scale))throw std::runtime_error("Nonfinite glare parameters");
  Restore restore;programs();auto source=gpu::g_frameBuffer.texture;if(!source)throw std::runtime_error("Glare has no current scene");
  const auto config=vi::configured_fb_size();float sourceLogicalW=float(config.x),sourceLogicalH=float(config.y);
  for(const auto& level:p.levels){
    if(!level.source_width||!level.source_height||level.source_width>sourceLogicalW||level.source_height>sourceLogicalH)throw std::runtime_error("Glare downsample exceeds preceding source");
    auto next=target(level.width,level.height);const std::array<float,4> rect={0,0,float(level.source_width)/sourceLogicalW,float(level.source_height)/sourceLogicalH};copy_target(source,next,rect,true,false);source=std::move(next);sourceLogicalW=level.width;sourceLogicalH=level.height;
  }
  if(source->width!=p.width||source->height!=p.height){auto cropped=target(p.width,p.height);const std::array<float,4> rect={0,0,float(p.width)/sourceLogicalW,float(p.height)/sourceLogicalH};copy_target(source,cropped,rect,false,false);source=std::move(cropped);}
  auto output=target(p.width,p.height);gl::bind_framebuffer(output->gl(),0);gl::set_viewport(0,0,p.width,p.height,0,1);gl::set_scissor_enabled(false);gl::apply_raster_state({});auto program=brightProgram->get();
  gl::uniform4(program,"thresholds",p.low_threshold,p.high_threshold,p.darken,p.brighten);gl::uniform4(program,"scale",p.shift_scale,0,0,0);gl::bind_texture(0,source->gl(),gl::sampler(gl::NearestClamp));gl::draw_fullscreen(program);publish(id,std::move(output));return true;
}
uint64_t scene_feedback_texture_version(const void* id,uint32_t w,uint32_t h){auto it=images.find(id);return it!=images.end()&&it->second.texture->width==w&&it->second.texture->height==h?it->second.revision:0;}
bool bind_scene_feedback_texture(uint32_t unit,const void* id,uint32_t w,uint32_t h,const NativeTextureSampler& s){
  owner();auto it=images.find(id);if(it==images.end()||it->second.texture->width!=w||it->second.texture->height!=h)return false;
  if(s.wrap_s||s.wrap_t||s.min_filter>1||s.mag_filter>1||s.lod_bias!=0)throw std::runtime_error("Unsupported scene feedback sampler");
  if(unit>=sampledImages.size())throw std::runtime_error("Scene feedback texture unit out of range");
  gl::SamplerDesc sampler=s.min_filter||s.mag_filter?gl::LinearClamp:gl::NearestClamp;sampler.minFilter=s.min_filter?GL_LINEAR:GL_NEAREST;sampler.magFilter=s.mag_filter?GL_LINEAR:GL_NEAREST;sampledImages[unit]=it->second.texture;gl::bind_texture(unit,it->second.texture->gl(),gl::sampler(sampler));return true;
}
bool clone_scene_feedback_texture(const void* destination,const void* source){owner();if(!aurora_native_frame_should_render())return false;auto it=images.find(source);if(it==images.end())return false;publish(destination,it->second.texture);return true;}
void clear_scene_feedback_textures(){owner();sampledImages.fill({});images.clear();copyPrograms.fill({});brightProgram.reset();}
}
