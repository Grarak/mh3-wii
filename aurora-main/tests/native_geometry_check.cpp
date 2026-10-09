// ROM-free pixel probe for direct attributes, texture modulation, reversed
// depth and frame-slot reuse. No GX API or command decoder participates.
#include <aurora/aurora.h>
#include <aurora/native_geometry.h>
#include <aurora/native_texture.hpp>
#include "gl/gl.hpp"
#include "gl/device.hpp"
#include "gx/gx.hpp"
#include "gx/shader_info.hpp"
#include "gxm/sampler_bias.hpp"
#include "gxm/alpha_compare.hpp"
#include "native_graphics.h"
// Standalone raster/pixel fixture has no model owner registry. Matrix-producer
// notifications are an explicit test boundary; production links model_pose.
void PublishNativeModelPositionMatrix(uint32_t,const float[12]) {}
void PublishNativeModelNormalMatrix(uint32_t,const float[9]) {}
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

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
// Compare the admitted no-bias path with the authored biased lookup over a
// complete framebuffer. Derivatives vary from magnification to minification;
// point and linear filters must each agree before bias may be omitted.
void single_mip_bias_probe() {
  namespace gl=aurora::gl;
  std::array<uint8_t,256> tiled{};
  for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
    const unsigned tile=(y/4)*2+x/4,index=(y%4)*4+x%4,at=tile*64;
    tiled[at+index*2]=255;tiled[at+index*2+1]=uint8_t(x*31);
    tiled[at+32+index*2]=uint8_t(y*31);
    tiled[at+33+index*2]=((x^y)&1)?255:0;
  }
  aurora::NativeTextureSource source;
  source.bytes=tiled;source.width=source.height=8;source.format=6;
  auto target=gl::create_texture(64,64,1,gl::Format::RGBA8,"single-mip bias comparison");
  gl::bind_framebuffer(target->gl(),0);gl::set_viewport(0,0,64,64,0,1);
  gl::set_scissor_enabled(false);gl::RasterState raster{};
  raster.cull=false;raster.depthTest=false;raster.blend=false;gl::apply_raster_state(raster);
  gl::ProgramDesc desc;
  desc.label="single-mip authored bias comparison";
  desc.origin="aurora-main/tests/native_geometry_check.cpp:single_mip_bias_probe";
  desc.vertex=gl::vertex_source(R"(
out vec2 fixture_uv;
uniform vec4 fixture_options;
void main() {
  vec2 xy=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));
  fixture_uv=xy*fixture_options.x+vec2(0.0625,0.09375);
  gl_Position=vec4(xy*2.0-1.0,0.0,1.0);
})");
  desc.fragment=gl::fragment_source(R"(
in vec2 fixture_uv;
uniform sampler2D tex0;
uniform vec4 fixture_bias;
layout(location=0) out vec4 fixture_color;
void main() { fixture_color=texture(tex0,fixture_uv,fixture_bias.x); }
)");
  desc.samplers={{"tex0",0}};gl::Program authored(desc);
  desc.label="single-mip admitted independent comparison";
  desc.fragment=gl::fragment_source(R"(
in vec2 fixture_uv;
uniform sampler2D tex0;
layout(location=0) out vec4 fixture_color;
void main() { fixture_color=texture(tex0,fixture_uv); }
)");
  gl::Program independent(desc);
  std::vector<uint8_t> reference(64*64*4),actual(reference.size());
  unsigned comparisons=0;
  for(unsigned filter=0;filter<6;++filter) for(float bias:{-4.f,-.0625f,.03125f,2.f,4.125f}) {
    aurora::NativeTextureSampler sampler;
    sampler.min_filter=filter;sampler.mag_filter=filter&1;sampler.lod_bias=bias;
    aurora::bind_native_texture(0,source,sampler);
    require(aurora::native_texture_shader_lod_bias(0)==0,"ineffective single-mip bias was retained");
    for(float scale:{.25f,1.f,16.f,256.f}) {
      gl::uniform4(authored.get(),"fixture_options",scale,0,0,0);
      gl::uniform4(authored.get(),"fixture_bias",bias,0,0,0);gl::draw_fullscreen(authored.get());
      gl::read_pixels(target->gl(),0,0,64,64,reference.data());
      gl::uniform4(independent.get(),"fixture_options",scale,0,0,0);gl::draw_fullscreen(independent.get());
      gl::read_pixels(target->gl(),0,0,64,64,actual.data());
      require(actual==reference,"single-mip independent pixels differ from authored bias");++comparisons;
    }
    sampler.mag_filter^=1;aurora::bind_native_texture(0,source,sampler);
    require(aurora::native_texture_shader_lod_bias(0)==bias,"unequal min/mag filters lost authored bias");
    sampler.mag_filter^=1;sampler.anisotropy=1;aurora::bind_native_texture(0,source,sampler);
    require(aurora::native_texture_shader_lod_bias(0)==bias,"anisotropic filtering lost authored bias");
  }
  // A biased LOD can cross the min/mag boundary even with only one mip. This
  // negative pixel control would fail if the admission tested only mip count.
  aurora::NativeTextureSampler unequal;
  unequal.mag_filter=1;unequal.lod_bias=4.125f;
  aurora::bind_native_texture(0,source,unequal);
  gl::uniform4(authored.get(),"fixture_options",1,0,0,0);
  gl::uniform4(authored.get(),"fixture_bias",unequal.lod_bias,0,0,0);gl::draw_fullscreen(authored.get());
  gl::read_pixels(target->gl(),0,0,64,64,reference.data());
  gl::uniform4(independent.get(),"fixture_options",1,0,0,0);gl::draw_fullscreen(independent.get());
  gl::read_pixels(target->gl(),0,0,64,64,actual.data());
  require(actual!=reference,"unequal-filter fixture did not exercise bias-sensitive pixels");
  require(glGetError()==GL_NO_ERROR,"single-mip bias probe emitted a GLES error");
  std::printf("Single-mip bias passed: %u identical complete framebuffers; unequal filters and anisotropy retain bias\n",comparisons);
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
  float texture[4]{4,4,0,0};
  for(unsigned i=0;i<2;++i) {
    texture[2]=i?aurora::native_texture_shader_lod_bias(0):0;
    std::memcpy(static_cast<uint8_t*>(uniforms.data)+at,texture,16);at+=16;
  }
  require(at<=uniforms.size,"fixture material uniforms exceed layout");
  std::memset(geometry.vertices,0,4*sizeof(*geometry.vertices));
  const float positions[4][2]{{-.08f,-.6f},{.08f,-.6f},{.08f,-.4f},{-.08f,-.4f}};
  for(unsigned i=0;i<4;++i) {
    auto& vertex=geometry.vertices[i];
    vertex.position[0]=positions[i][0];vertex.position[1]=positions[i][1];
    vertex.position[2]=.8f;vertex.position[3]=1;
    vertex.normal[0]=1;vertex.texcoord[0][0]=(i==1 || i==2)?1.f:0.f;
    vertex.texcoord[0][1]=i>=2?1.f:0.f;vertex.texcoord[0][2]=1;
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
    std::string floatAlpha="#version 300 es\nif (alphaCompare > 128.0 && alphaCompare != 0.0) {}\n";
    std::array<float,4> references{};
    require(aurora::gxm::parameterize_alpha_references(floatAlpha,references) &&
            references[0]==128 && references[1]==0 &&
            floatAlpha.find("alphaCompare > aur_alpha_refs.x")!=std::string::npos &&
            floatAlpha.find("alphaCompare != aur_alpha_refs.y")!=std::string::npos &&
            floatAlpha.find("u32(")==std::string::npos,
            "native float alpha parameters changed comparison type");
    const auto materialInfo=aurora::gx::build_shader_info(materialConfig);
    auto materialDescription=aurora::gx::build_shader_description(materialConfig,true,1);
    const auto exactDescription=aurora::gx::build_shader_description(materialConfig,true,1,false);
    require(materialDescription.fragment.find("prev = tev_overflow_vec4f(prev)")==std::string::npos,
            "ordinary native material retained wrap emulation");
    require(exactDescription.fragment.find("prev = tev_overflow_vec4f(prev)")!=std::string::npos,
            "exact native reference was approximated");
    for(unsigned guard=0;guard<4;++guard) {
      auto guarded=materialConfig;
      if(guard==0) {guarded.alphaCompare.comp0=GX_GREATER;guarded.alphaCompare.ref0=128;}
      if(guard==1) guarded.tevStages[0].colorOp.clamp=false;
      if(guard==2) guarded.tevStages[0].colorOp.op=GX_TEV_COMP_RGB8_GT;
      if(guard==3) guarded.tevStages[0].colorPass.d=GX_CC_C0;
      const auto description=aurora::gx::build_shader_description(guarded,true,1);
      require(description.fragment.find("prev = tev_overflow_vec4f(prev)")==std::string::npos,
              "native material silently fell back to integer emulation");
      aurora::gl::Program checked(description);checked.get();
    }
    auto rejectConfig=materialConfig;rejectConfig.alphaCompare.op=GX_AOP_XOR;
    const auto rejectDescription=aurora::gx::build_shader_description(rejectConfig,true,1);
    require(rejectDescription.fragment.find("discard;")!=std::string::npos,
            "ALWAYS XOR ALWAYS did not reject fragments");
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
    auto exactProgram=std::make_shared<aurora::gl::Program>(exactDescription);
    auto biasedProgram=std::make_shared<aurora::gl::Program>(biasedDescription);
    for(int code=0;code<64;++code)
      require(aurora::gxm::sampler_bias_encoding(float(code-31)/8)==code,"GXM bias encoding mismatch");
    for(float bias:{-4.f,4.125f,.0625f,-.0625f})
      require(aurora::gxm::sampler_bias_encoding(bias)==-1,"GXM bias silently rounded");
    auto samplerA=aurora::gl::NearestClamp,samplerB=samplerA;
    samplerB.lodBias=1;
    require(!(samplerA==samplerB),"sampler cache ignores LOD bias");
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
        if(frame==0) {
          single_mip_bias_probe();
          aurora::gl::bind_framebuffer(target.texture->gl(),aurora::gpu::g_depthBuffer.texture->gl());
          aurora::gl::set_viewport(0,0,64,64,0,1);aurora::gl::apply_raster_state(raster);
          std::array<uint8_t,192> mipBytes{};
          for(unsigned mip=0;mip<3;++mip) for(unsigned texel=0;texel<16;++texel) {
            auto* p=mipBytes.data()+64*mip;
            p[texel*2]=255;p[texel*2+1]=mip==0?255:0;
            p[32+texel*2]=mip==1?255:0;p[33+texel*2]=mip==2?255:0;
          }
          aurora::NativeTextureSource mipSource;
          mipSource.bytes=mipBytes;mipSource.width=mipSource.height=4;mipSource.levels=3;mipSource.format=6;
          aurora::NativeTextureSampler mipSampler;
          mipSampler.min_filter=2;mipSampler.max_lod=2;
          aurora::bind_native_texture(0,mipSource,mipSampler);
          material_quad(biasedProgram->get(),materialInfo);pixel(32,48,{255,0,0,255});
          aurora::gl::clear_depth(0);
          mipSampler.lod_bias=2;
          aurora::bind_native_texture(0,mipSource,mipSampler);
          require(aurora::native_texture_shader_lod_bias(0)==2,"GLES lost shader LOD bias");
          material_quad(biasedProgram->get(),materialInfo);pixel(32,48,{0,0,255,255});
          aurora::gl::clear_color(clear);aurora::gl::clear_depth(0);
        }
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
        uint8_t approximate[4]{},exact[4]{};
        aurora::gl::read_pixels(target.texture->gl(),32,48,1,1,approximate);
        aurora::gl::clear_depth(0);
        material_quad(exactProgram->get(),materialInfo);
        aurora::gl::read_pixels(target.texture->gl(),32,48,1,1,exact);
        for(unsigned channel=0;channel<4;++channel)
          require(std::abs(int(approximate[channel])-int(exact[channel]))<=1,
                  "native float/reference material differs by more than one byte step");
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
