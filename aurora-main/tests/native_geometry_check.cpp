// ROM-free pixel probe for direct attributes, texture modulation, reversed
// depth and frame-slot reuse. No GX API or command decoder participates.
#include <aurora/aurora.h>
#include <aurora/native_geometry.h>
#include <aurora/native_texture.hpp>
#include "gl/gl.hpp"
#include "gl/device.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void quad(float left, float right, float bottom, float top, float z,
          const std::array<uint8_t,4>& color, bool textured) {
  AuroraNativeGeometry geometry{};
  require(aurora_allocate_native_geometry(4,6,&geometry), "native allocation failed");
  const float positions[4][2]{{left,bottom},{right,bottom},{right,top},{left,top}};
  const float uv[4][2]{{0,0},{1,0},{1,1},{0,1}};
  for (unsigned i=0;i<4;++i) {
    auto& vertex=geometry.vertices[i];
    vertex.position[0]=positions[i][0];vertex.position[1]=positions[i][1];
    vertex.position[2]=z;vertex.position[3]=1;
    std::memcpy(vertex.uv,uv[i],sizeof(vertex.uv));
    std::memcpy(vertex.color,color.data(),sizeof(vertex.color));
  }
  const uint16_t indices[]{0,1,2,0,2,3};
  std::memcpy(geometry.indices,indices,sizeof(indices));
  aurora_submit_native_geometry(&geometry,textured);
}
void pixel(unsigned x,unsigned y,const std::array<uint8_t,4>& expected) {
  uint8_t actual[4]{};
  aurora::gl::read_pixels(aurora::gpu::g_frameBuffer.texture->gl(),x,y,1,1,actual);
  for (unsigned i=0;i<4;++i) if (std::abs(int(actual[i])-int(expected[i]))>1) {
    std::fprintf(stderr,"pixel %u,%u: %u,%u,%u,%u expected %u,%u,%u,%u\n",x,y,
      actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);
    throw std::runtime_error("native geometry pixel mismatch");
  }
}
}
int main(int argc,char** argv) {
  try {
    const std::string path=argc>1?argv[1]:"/tmp/mh3-native-geometry-check";
    std::filesystem::create_directories(path);
    require(aurora_configure_native_target(64,64),"target setup failed");
    AuroraConfig config{};
    config.appName="MH3 native geometry check";
    config.userPath=config.cachePath=path.c_str();
    config.desiredBackend=BACKEND_OPENGLES;
    config.windowWidth=config.windowHeight=64;config.msaa=1;
    const auto info=aurora_initialize(argc,argv,&config);
    require(info.initializationStatus==AURORA_INITIALIZATION_SUCCESS,"graphics initialization failed");
    struct Shutdown { ~Shutdown(){aurora_shutdown();} } shutdown;
    aurora_set_frameskip(true);
    require(aurora::native_texture_source_size(14,9,9,1)==128,"CMPR tile size mismatch");
    require(aurora::native_texture_source_size(6,4,4,3)==192,"small mip tile size mismatch");
    std::array<uint8_t,64> texels{};
    const auto fill=[&](uint8_t red) {
      for(unsigned i=0;i<16;++i) {
        texels[i*2]=255;texels[i*2+1]=red;
        texels[32+i*2]=255;texels[33+i*2]=64;
      }
    };
    fill(64);
    aurora::NativeTextureSource source;
    source.bytes=texels;source.width=source.height=4;source.format=6;
    source.generation=1;source.mutable_source=true;
    std::array<uint8_t,32> indices{},palette{};
    aurora::NativeTextureSource indexed;
    indexed.bytes=indices;indexed.palette=palette;indexed.width=indexed.height=8;
    indexed.format=8;indexed.palette_format=1;indexed.palette_entries=16;
    unsigned rendered=0,slots=0;
    for (unsigned frame=0;frame<12;++frame) {
      aurora_update();
      const bool draw=aurora_begin_native_frame();
      if (draw) {
        slots|=1u<<aurora_native_frame_slot();++rendered;
        const auto& target=aurora::gpu::g_frameBuffer;
        aurora::gl::bind_framebuffer(target.texture->gl(),aurora::gpu::g_depthBuffer.texture->gl());
        aurora::gl::set_viewport(0,0,64,64,0,1);
        aurora::gl::set_scissor_enabled(false);
        aurora::gl::RasterState raster{};
        raster.depthTest=true;raster.depthWrite=true;raster.depthFunc=GL_GREATER;
        raster.cull=false;raster.blend=false;raster.colorMask={true,true,true,true};
        aurora::gl::apply_raster_state(raster);
        const float clear[]{20.f/255,40.f/255,80.f/255,1};
        aurora::gl::clear_color(clear);aurora::gl::clear_depth(0);
        quad(-.9f,-.1f,.1f,.9f,0,{255,0,0,255},false);
        if(rendered==2) {fill(128);source.generation=2;}
        // Deliberately mutate the staging bytes without publishing a new
        // revision: sampling must retain revision 2, with no content inspection.
        if(rendered==3) fill(0);
        if(rendered<=2) aurora::bind_native_texture(0,source,{});
        quad(.1f,.9f,.1f,.9f,0,{255,128,255,255},true);
        quad(-.9f,.9f,-.9f,-.1f,.5f,{0,255,0,255},false);
        quad(-.9f,.9f,-.9f,-.1f,-.5f,{0,0,255,255},false);
        // Two versions of one palette in the same frame must retain separate
        // sampled images; publishing the second cannot overwrite the first.
        palette[0]=0;palette[1]=31;indexed.palette_generation=rendered*2;
        aurora::bind_native_texture(0,indexed,{});
        quad(-.9f,-.1f,-.9f,-.1f,.75f,{255,255,255,255},true);
        palette[0]=248;palette[1]=0;++indexed.palette_generation;
        aurora::bind_native_texture(0,indexed,{});
        quad(.1f,.9f,-.9f,-.1f,.75f,{255,255,255,255},true);
        // Preserve this application binding across the present pass; the next
        // frame deliberately draws before issuing another texture bind.
        aurora::bind_native_texture(0,source,{});
      } else {
        AuroraNativeGeometry unavailable{};
        require(!aurora_allocate_native_geometry(4,6,&unavailable),"skipped frame exposed draw memory");
      }
      aurora_end_native_frame();
    }
    require(rendered>0 && slots==7,"did not exercise all three draw slots");
    pixel(0,0,{20,40,80,255});
    pixel(16,16,{255,0,0,255});
    pixel(48,16,{128,128,64,255});
    pixel(32,48,{0,255,0,255});
    pixel(16,48,{0,0,255,255});
    pixel(48,48,{255,0,0,255});
    std::printf("Native geometry passed: %u frames, slot mask %u, solid/texture/depth pixels, image/palette revisions\n",rendered,slots);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr,"Native geometry check failed: %s\n",error.what());
    return 1;
  }
}
