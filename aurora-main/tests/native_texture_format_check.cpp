// ROM-free GLES format/sampler probe. Readback exists only in this executable.
#include <aurora/aurora.h>
#include <aurora/native_geometry.h>
#include <aurora/native_texture.hpp>
#include "gl/device.hpp"
#include "gl/gl.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
std::vector<uint8_t> read(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);require(bool(f),"Missing external texture reference");return {std::istreambuf_iterator<char>(f),{}};}
void quad(float u,float v){
  AuroraNativeGeometry geometry{};require(aurora_allocate_native_geometry(4,6,&geometry),"Texture probe geometry capacity exhausted");
  const float position[4][2]={{-1,1},{1,1},{1,-1},{-1,-1}};

  for(unsigned i=0;i<4;i++){auto& vertex=geometry.vertices[i];vertex.position[0]=position[i][0];vertex.position[1]=position[i][1];vertex.position[2]=0;vertex.position[3]=1;vertex.uv[0]=u;vertex.uv[1]=v;std::memset(vertex.color,255,4);}
  const uint16_t indices[6]={0,1,2,0,2,3};std::memcpy(geometry.indices,indices,12);aurora_submit_native_geometry(&geometry,true);
}

void pixel(const uint8_t* expected,unsigned tolerance){
  uint8_t actual[4];aurora::gl::read_pixels(aurora::gpu::g_frameBuffer.texture->gl(),32,32,1,1,actual);

  for(unsigned c=0;c<4;c++)if(unsigned(std::abs(int(actual[c])-int(expected[c])))>tolerance){std::fprintf(stderr,"Pixel %u,%u,%u,%u expected %u,%u,%u,%u\n",actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);throw std::runtime_error("Native texture format/sampler pixel differs");}
}
}

int main(int argc,char** argv){
  try{
    require(argc>=2,"Usage: native_texture_format_check FIXTURE_DIR [CACHE_DIR]");const std::filesystem::path directory=argv[1];
    const std::string cache=argc>=3?argv[2]:"/tmp/mh3-native-texture-formats";std::filesystem::create_directories(cache);
    require(aurora_configure_native_target(64,64),"Texture probe target configuration failed");AuroraConfig config{};config.appName="MH3 native texture format check";config.userPath=config.cachePath=cache.c_str();config.desiredBackend=BACKEND_OPENGLES;config.windowWidth=config.windowHeight=64;config.msaa=1;
    require(aurora_initialize(argc,argv,&config).initializationStatus==AURORA_INITIALIZATION_SUCCESS,"Texture probe renderer initialization failed");struct Shutdown{~Shutdown(){aurora_shutdown();}}shutdown;
    aurora_set_frameskip(false);aurora::gl::open_native_texture_pack((directory/"native-textures.bin").c_str());unsigned samples=0;

    for(uint32_t asset=1;asset<=11;asset++){
      aurora_update();require(aurora_begin_native_frame(),"Texture probe frame unexpectedly skipped");
      const auto& target=aurora::gpu::g_frameBuffer;aurora::gl::bind_framebuffer(target.texture->gl(),aurora::gpu::g_depthBuffer.texture->gl());aurora::gl::set_viewport(0,0,64,64,0,1);aurora::gl::set_scissor_enabled(false);aurora::gl::apply_raster_state({});
      auto texture=aurora::gl::native_texture_asset(asset);const auto info=aurora::gl::native_texture_info(asset);

      for(unsigned mip=0;mip<info.mips;mip++){
        const auto w=std::max(info.width>>mip,1u),h=std::max(info.height>>mip,1u);const auto expected=read(directory/("asset-"+std::to_string(asset)+"-mip-"+std::to_string(mip)+".rgba"));require(expected.size()==size_t(w)*h*4,"Texture reference dimensions differ");
        aurora::gl::SamplerDesc desc=aurora::gl::NearestClamp;desc.minFilter=GL_NEAREST_MIPMAP_NEAREST;desc.minLod=desc.maxLod=float(mip);

        for(const auto point:std::array<std::array<unsigned,2>,3>{{{0,0},{w/2,h/2},{w-1,h-1}}}){
          const float u=(point[0]+.5f)/w,v=(point[1]+.5f)/h;

          for(unsigned wrap=0;wrap<3;wrap++){
            desc.wrapS=desc.wrapT=wrap==0?GL_CLAMP_TO_EDGE:wrap==1?GL_REPEAT:GL_MIRRORED_REPEAT;
            aurora::gl::bind_texture(0,texture->gl(),aurora::gl::sampler(desc));quad(wrap==0?u:wrap==1?u+1:2-u,wrap==0?v:wrap==1?v-1:2-v);
            pixel(expected.data()+(size_t(point[1])*w+point[0])*4,info.format==aurora::gl::Format::RGB565?1:0);samples++;
          }
        }

        desc.wrapS=desc.wrapT=GL_CLAMP_TO_EDGE;desc.minFilter=GL_LINEAR_MIPMAP_NEAREST;desc.magFilter=GL_LINEAR;
        aurora::gl::bind_texture(0,texture->gl(),aurora::gl::sampler(desc));quad(w>1?1.f/w:.5f,.5f/h);uint8_t average[4];for(unsigned c=0;c<4;c++)average[c]=uint8_t((unsigned(expected[c])+expected[(w>1?4:0)+c]+1)/2);pixel(average,1);samples++;
      }

      aurora_end_native_frame();
    }

    aurora_update();require(aurora_begin_native_frame(),"Producer revision probe frame skipped");
    aurora::gl::bind_framebuffer(aurora::gpu::g_frameBuffer.texture->gl(),aurora::gpu::g_depthBuffer.texture->gl());
    aurora::gl::set_viewport(0,0,64,64,0,1);aurora::gl::set_scissor_enabled(false);aurora::gl::apply_raster_state({});
    aurora::NativeTextureSampler sampling{};sampling.wrap_s=sampling.wrap_t=2;
    std::array<uint8_t,32> image;image.fill(13);aurora::NativeTextureSource source{};
    source.bytes=image;source.width=8;source.height=4;source.format=1;source.asset=2;source.generation=1;
    aurora::bind_native_texture(0,source,sampling);quad(.5f,.5f);const uint8_t original[]{13,13,13,13};pixel(original,0);
    image.fill(73);source.generation=2;aurora::bind_native_texture(0,source,sampling);quad(.5f,.5f);const uint8_t updated[]{73,73,73,73};pixel(updated,0);
    std::array<uint8_t,32> indices{};std::array<uint8_t,4> palette{0xf8,0,0x07,0xe0};
    source={};source.bytes=indices;source.palette=palette;source.width=source.height=8;source.format=8;
    source.asset=2;source.generation=1;source.palette_generation=1;source.palette_format=1;source.palette_entries=2;
    aurora::bind_native_texture(0,source,sampling);quad(.5f,.5f);const uint8_t red[]{255,0,0,255};pixel(red,0);
    palette[0]=0;palette[1]=0x1f;source.palette_generation=2;
    aurora::bind_native_texture(0,source,sampling);quad(.5f,.5f);const uint8_t blue[]{0,0,255,255};pixel(blue,0);
    indices.fill(0x11);source.generation=2;
    aurora::bind_native_texture(0,source,sampling);quad(.5f,.5f);const uint8_t green[]{0,255,0,255};pixel(green,0);
    aurora::clear_native_textures();aurora_end_native_frame();
    std::printf("GLES native texture formats passed: 11 assets,55 mips,%u point/wrap/linear samples; live image/palette revisions PASS\n",samples);return 0;
  }catch(const std::exception& e){std::fprintf(stderr,"Native texture format probe failed: %s\n",e.what());return 1;}
}
