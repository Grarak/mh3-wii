// ROM-free GPU feedback probe. Pixel reads occur only here as diagnostics.
#include <aurora/aurora.h>
#include <aurora/native_geometry.h>
#include <aurora/native_scene_feedback.hpp>
#include <aurora/generated_texture.hpp>
#include "gl/device.hpp"
#include "gl/gl.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace {
void require(bool value,const char *reason){if(!value)throw std::runtime_error(reason);}
void pixel(std::array<uint8_t,4> expected){
  uint8_t actual[4];aurora::gl::read_pixels(aurora::gpu::g_frameBuffer.texture->gl(),32,32,1,1,actual);
  for(unsigned i=0;i<4;i++)if(std::abs(int(actual[i])-int(expected[i]))>1){std::fprintf(stderr,"Pixel: %u,%u,%u,%u expected %u,%u,%u,%u\n",actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);throw std::runtime_error("Native scene feedback pixel differs");}
}
void clear(float r,float g,float b){const float color[4]={r,g,b,1};aurora::gl::clear_color(color);}
void quad(){
  AuroraNativeGeometry geometry{};require(aurora_allocate_native_geometry(4,6,&geometry),"feedback probe geometry allocation failed");
  const float position[4][2]={{-1,1},{1,1},{1,-1},{-1,-1}},uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
  for(unsigned i=0;i<4;i++){auto &v=geometry.vertices[i];v.position[0]=position[i][0];v.position[1]=position[i][1];v.position[2]=0;v.position[3]=1;std::memcpy(v.uv,uv[i],8);std::memset(v.color,255,4);}
  const uint16_t indices[6]={0,1,2,0,2,3};std::memcpy(geometry.indices,indices,12);aurora_submit_native_geometry(&geometry,true);
}
void composition(){
  aurora::MaterialComposition material;for(auto &source:material.sources){source.width=source.height=128;source.format=6;source.bytes.resize(128*128*4);}
  // Wii RGBA8 layout: per4x4 tile, 32 AR bytes then32 GB bytes.
  for(size_t tile=0;tile<material.sources[0].bytes.size();tile+=64)for(unsigned i=0;i<16;i++){
    material.sources[0].bytes[tile+2*i]=255;material.sources[0].bytes[tile+2*i+1]=100;
    material.sources[1].bytes[tile+2*i]=128;material.sources[1].bytes[tile+2*i+1]=200;
  }
  material.opacity=.5f;static uint8_t identity;
  aurora::publish_generated_material(123,&identity,std::move(material));
}
}
int main(int argc,char **argv){
  try {
    const std::string cache=argc>1?argv[1]:"/tmp/mh3-native-scene-feedback";std::filesystem::create_directories(cache);
    require(aurora_configure_native_target(64,64),"feedback target configuration failed");AuroraConfig config{};config.appName="MH3 native scene feedback check";config.userPath=config.cachePath=cache.c_str();config.desiredBackend=BACKEND_OPENGLES;config.windowWidth=config.windowHeight=64;config.msaa=1;
    require(aurora_initialize(argc,argv,&config).initializationStatus==AURORA_INITIALIZATION_SUCCESS,"feedback renderer initialization failed");
    struct Shutdown {~Shutdown(){aurora::clear_scene_feedback_textures();aurora::clear_generated_textures();aurora_shutdown();}} shutdown;
    aurora_set_frameskip(true);static uint8_t history,glare,clone;unsigned rendered=0;
    for(unsigned frame=0;frame<6;frame++){
      aurora_update();const bool draw=aurora_begin_native_frame();if(draw){
        rendered++;const auto &target=aurora::gpu::g_frameBuffer;aurora::gl::bind_framebuffer(target.texture->gl(),aurora::gpu::g_depthBuffer.texture->gl());aurora::gl::set_viewport(0,0,64,64,0,1);aurora::gl::set_scissor(2,3,60,59);aurora::gl::set_scissor_enabled(false);aurora::gl::apply_raster_state({});
        clear(1,0,0);const aurora::SceneCaptureParameters capture{{0,0,64,64},32,32,4,true};require(aurora::publish_scene_capture(&history,capture),"history capture not published");const auto oldRevision=aurora::scene_feedback_texture_version(&history,32,32);aurora::NativeTextureSampler sampling;sampling.min_filter=sampling.mag_filter=1;require(aurora::bind_scene_feedback_texture(0,&history,32,32,sampling),"history texture not bound");
        clear(0,0,1);require(aurora::publish_scene_capture(&history,capture),"new history capture not published");require(aurora::scene_feedback_texture_version(&history,32,32)!=oldRevision,"feedback revision did not advance");
        // The old published image remains bound across replacement and passes.
        quad();pixel({255,0,0,255});require(aurora::bind_scene_feedback_texture(0,&history,32,32,sampling),"new history not rebound");quad();pixel({0,0,255,255});
        require(aurora::clone_scene_feedback_texture(&clone,&history),"saved snapshot clone failed");clear(1,0,0);
        aurora::SceneGlareParameters bloom;bloom.levels={{64,64,32,32},{32,32,16,16}};bloom.width=bloom.height=16;bloom.low_threshold=64.f/255;bloom.high_threshold=192.f/255;bloom.darken=bloom.brighten=.5f;bloom.shift_scale=.5f;require(aurora::publish_scene_glare(&glare,bloom),"glare texture not published");require(aurora::bind_scene_feedback_texture(0,&glare,16,16,sampling),"glare texture not bound");quad();pixel({38,38,38,255});
        const auto saved=aurora::gl::save_offscreen_state();composition();const auto after=aurora::gl::save_offscreen_state();require(saved.framebuffer==after.framebuffer&&saved.viewport==after.viewport&&saved.scissor==after.scissor&&saved.scissorEnabled==after.scissorEnabled&&saved.textures==after.textures&&saved.samplers==after.samplers&&saved.program==after.program&&saved.vertexArray==after.vertexArray,"generated composition leaked native state");
        require(aurora::bind_scene_feedback_texture(0,&clone,32,32,sampling),"saved clone not bound");quad();pixel({0,0,255,255});
      }aurora_end_native_frame();
    }
    require(rendered==6,"feedback probe skipped host frames");std::printf("Native scene feedback passed: %u frames, history revisions, retained bindings, grayscale glare, state restoration\n",rendered);return 0;
  } catch(const std::exception &e){std::fprintf(stderr,"Native scene feedback failed: %s\n",e.what());return 1;}
}
