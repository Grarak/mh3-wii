// ROM-free pixel probe for direct attributes, texture modulation, reversed
// depth and frame-slot reuse. No GX API or command decoder participates.
#include <aurora/aurora.h>
#include <aurora/native_geometry.h>
#include <aurora/native_texture.hpp>
#include "gl/gl.hpp"
#include "gl/device.hpp"
#include "gx/gx.hpp"
#include "gx/shader_info.hpp"
#include "native_graphics.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

// This probe links the real runtime state publisher without the game/ROM.
// Suppression models a logical frame which was not admitted for rendering.
static bool suppressDrawing=false;
bool NativeGraphicsFrameShouldRender() {
  return !suppressDrawing && aurora_native_frame_should_render();
}

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void check_state_restore(unsigned frame) {
  GLint oldViewport[4]{},oldScissor[4]{};
  glGetIntegerv(GL_VIEWPORT,oldViewport);glGetIntegerv(GL_SCISSOR_BOX,oldScissor);
  const GLboolean oldDepthWrite=[] {GLboolean value;glGetBooleanv(GL_DEPTH_WRITEMASK,&value);return value;}();
  suppressDrawing=true;
  const float side=8+frame;
  SetNativeViewport(0,0,side,side,.25f,.75f);
  SetNativeScissor(1,1,4,4);
  const mh3_raster_state raster{0,1,0,3,1,3,0,0,1,0};
  SetNativeRaster(raster);
  RestoreNativeRenderState();
  GLint afterViewport[4]{},afterScissor[4]{};GLboolean afterDepthWrite;
  glGetIntegerv(GL_VIEWPORT,afterViewport);glGetIntegerv(GL_SCISSOR_BOX,afterScissor);
  glGetBooleanv(GL_DEPTH_WRITEMASK,&afterDepthWrite);
  require(std::memcmp(oldViewport,afterViewport,sizeof(oldViewport))==0 &&
          std::memcmp(oldScissor,afterScissor,sizeof(oldScissor))==0 && oldDepthWrite==afterDepthWrite,
          "suppressed native state touched GPU state");
  suppressDrawing=false;
  RestoreNativeRenderState();
  glGetIntegerv(GL_VIEWPORT,afterViewport);glGetIntegerv(GL_SCISSOR_BOX,afterScissor);
  glGetBooleanv(GL_DEPTH_WRITEMASK,&afterDepthWrite);
  const float sx=float(aurora::gpu::g_frameBuffer.width)/64;
  const float sy=float(aurora::gpu::g_frameBuffer.height)/64;
  require(afterViewport[0]==0 && afterViewport[1]==0 &&
          afterViewport[2]==std::lround(side*sx) && afterViewport[3]==std::lround(side*sy),
          "native viewport from suppressed frame was lost");
  require(glIsEnabled(GL_SCISSOR_TEST) && afterScissor[0]==std::floor(sx) &&
          afterScissor[1]==std::floor(sy) &&
          afterScissor[2]==std::ceil(5*sx)-std::floor(sx) &&
          afterScissor[3]==std::ceil(5*sy)-std::floor(sy),
          "native scissor from suppressed frame was lost");
  GLboolean mask[4]{};glGetBooleanv(GL_COLOR_WRITEMASK,mask);
  require(!afterDepthWrite && mask[0] && mask[1] && mask[2] && !mask[3],
          "native raster from suppressed frame was lost");
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
aurora::gx::ShaderConfig material_config() {
  using namespace aurora::gx;
  ShaderConfig config;
  config.attrs[GX_VA_POS]={GX_DIRECT,3,GX_F32};
  config.attrs[GX_VA_NRM]={GX_DIRECT,3,GX_F32};
  config.attrs[GX_VA_CLR0]={GX_DIRECT,4,GX_RGBA8};
  config.attrs[GX_VA_TEX0]={GX_DIRECT,2,GX_F32};
  config.colorChannels[0].matSrc=GX_SRC_VTX;
  config.colorChannels[GX_ALPHA0].matSrc=GX_SRC_VTX;
  config.tevStageCount=1;config.numTexGens=1;
  auto& stage=config.tevStages[0];
  stage.texMapId=GX_TEXMAP0;stage.texCoordId=GX_TEXCOORD0;stage.channelId=GX_COLOR0A0;
  stage.colorPass={GX_CC_ZERO,GX_CC_TEXC,GX_CC_RASC,GX_CC_ZERO};
  stage.alphaPass={GX_CA_ZERO,GX_CA_TEXA,GX_CA_RASA,GX_CA_ZERO};
  return config;
}
void material_quad(GLuint program,const aurora::gx::ShaderInfo& info) {
  AuroraNativeMaterialGeometry geometry{};
  AuroraNativeUniforms uniforms{};
  require(aurora_allocate_native_material_geometry(4,6,&geometry),"material allocation failed");
  require(aurora_allocate_native_uniforms(info.uniformSize,&uniforms),"material uniforms allocation failed");
  std::memset(uniforms.data,0,uniforms.size);
  size_t at=144+48*(info.matrixLayout.postexCount+info.matrixLayout.nrmCount);
  const float texture[4]{4,4,0,0};
  for(unsigned i=0;i<2;++i) {std::memcpy(static_cast<uint8_t*>(uniforms.data)+at,texture,16);at+=16;}
  require(at<=uniforms.size,"fixture material uniforms exceed layout");
  std::memset(geometry.vertices,0,4*sizeof(*geometry.vertices));
  const float positions[4][2]{{-.08f,-.6f},{.08f,-.6f},{.08f,-.4f},{-.08f,-.4f}};
  for(unsigned i=0;i<4;++i) {
    auto& vertex=geometry.vertices[i];
    vertex.position[0]=positions[i][0];vertex.position[1]=positions[i][1];
    vertex.position[2]=.8f;vertex.position[3]=1;
    vertex.normal[0]=1;vertex.texcoord[0][0]=vertex.texcoord[0][1]=.5f;vertex.texcoord[0][2]=1;
    const uint8_t color[]{255,128,255,255};std::memcpy(vertex.color[0],color,4);
  }
  const uint16_t indices[]{0,1,2,0,2,3};std::memcpy(geometry.indices,indices,sizeof(indices));
  aurora_submit_native_material_geometry(&geometry,program,&uniforms);
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
    const auto materialConfig=material_config();
    const auto materialInfo=aurora::gx::build_shader_info(materialConfig);
    auto materialDescription=aurora::gx::build_shader_description(materialConfig,true,1);
    require(materialDescription.vertex.find("uniform highp usampler2D vbuf")==std::string::npos,
            "native material retained vertex word fetch binding");
    require(materialDescription.fragment.find("tex0_fixed_uv")==std::string::npos,
            "native ordinary texture retained fixed-point UV reconstruction");
    require(materialDescription.fragment.find("texture(tex0, tex0_uv);")!=std::string::npos,
            "zero-bias native sample retained dynamic bias");
    const auto biasedDescription=aurora::gx::build_shader_description(materialConfig,true);
    require(biasedDescription.fragment.find("texture(tex0, tex0_uv, ubuf.tex0_size_bias.z)")!=std::string::npos,
            "unspecialized native sample lost sampler bias");
    auto materialProgram=std::make_shared<aurora::gl::Program>(std::move(materialDescription));
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
        check_state_restore(frame);
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
        material_quad(materialProgram->get(),materialInfo);
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
    pixel(32,40,{0,255,0,255});
    pixel(32,48,{128,128,64,255});
    pixel(16,48,{0,0,255,255});
    pixel(48,48,{255,0,0,255});
    std::printf("Native geometry passed: %u frames, slot mask %u, solid/texture/depth pixels, image/palette revisions\n",rendered,slots);
    materialProgram.reset();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr,"Native geometry check failed: %s\n",error.what());
    return 1;
  }
}
