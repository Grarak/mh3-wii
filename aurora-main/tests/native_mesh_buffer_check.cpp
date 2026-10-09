// Host-only GPU storage check; readback is diagnostic, never a game operation.
#include <aurora/aurora.h>
#include "gl/gl.hpp"
#include "gl/device.hpp"
#include "gx/gx.hpp"
#include "gx/shader_info.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

static void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
// Diagnostic-only CPU reference bytes are built once. Production never
// expands resident vertices; this compares its actual palette shader equation.
static void indexed_palette_probe(const std::filesystem::path& path) {
  using Matrix=std::array<float,12>;
  const Matrix identity{1,0,0,0,0,1,0,0,0,0,1,0};
  std::array<Matrix,20> matrices;matrices.fill(identity);
  matrices[9][3]=matrices[9][7]=.125f;
  matrices[10]={.5f,0,0,.25f,0,.5f,0,.25f,0,0,1,0};
  matrices[11]={.5f,0,0,.25f,0,.5f,0,.25f,0,0,1,0};
  matrices[19]={.5f,0,0,.5f,0,.5f,0,.25f,0,0,1,0};
  const Matrix post{.5f,0,0,.25f,0,.5f,0,.25f,0,0,1,0};
  const auto transform=[](const Matrix& matrix,const std::array<float,4>& value) {
    std::array<float,3> out{};
    for(unsigned r=0;r<3;++r) for(unsigned c=0;c<4;++c) out[r]+=matrix[r*4+c]*value[c];
    return out;
  };
  struct Vertex {float position[3],uv[2];uint8_t selectors[9],pad[3];};
  static_assert(sizeof(Vertex)==32);
  const float xy[][2]{{-.75f,-.75f},{.75f,-.75f},{-.75f,.75f},
                     {.75f,-.75f},{.75f,.75f},{-.75f,.75f}};
  const uint8_t pn[]{0,9,0,9,0,9},tex0[]{10,19,20,19,10,20},tex7[]{0,9,20,9,0,20};
  std::array<Vertex,24> vertices{};
  for(unsigned i=0;i<6;++i) {
    auto& vertex=vertices[i];vertex.position[0]=xy[i][0];vertex.position[1]=xy[i][1];
    vertex.uv[0]=(xy[i][0]+.75f)/1.5f;vertex.uv[1]=(xy[i][1]+.75f)/1.5f;
    vertex.selectors[0]=pn[i];vertex.selectors[1]=tex0[i];vertex.selectors[4]=20;vertex.selectors[8]=tex7[i];
    for(unsigned pass=0;pass<3;++pass) {
      auto& reference=vertices[6*(pass+1)+i];
      const auto position=transform(matrices[pn[i]],{vertex.position[0],vertex.position[1],0,1});
      std::memcpy(reference.position,position.data(),12);
      const unsigned selector=pass==0?tex0[i]:pass==1?11:tex7[i];
      const auto main=selector==20?std::array<float,3>{vertex.uv[0],vertex.uv[1],1}:
          transform(matrices[selector],{vertex.uv[0],vertex.uv[1],1,1});
      const auto uv=transform(post,{main[0],main[1],1,1});
      reference.uv[0]=uv[0];reference.uv[1]=uv[1];
    }
  }
  std::array<uint8_t,800> payload{};
  std::memcpy(payload.data(),vertices.data(),sizeof vertices);
  const uint16_t indices[]{0,1,2,3,4,5};std::memcpy(payload.data()+768,indices,sizeof indices);
  FILE* file=std::fopen(path.c_str(),"wb");require(file,"cannot create indexed palette fixture");
  const auto written=std::fwrite(payload.data(),1,payload.size(),file);std::fclose(file);
  require(written==payload.size(),"indexed palette fixture write failed");
  aurora::gl::NativeMeshBuffer buffer(path.c_str(),0,payload.size());
  using F=aurora::gl::MeshAttributeFormat;
  const aurora::gl::MeshLayout indexed{32,{{0,0,3,F::F32},{12,4,2,F::F32},
      {20,12,4,F::U8},{24,13,4,F::U8},{28,14,1,F::U8}},uint16_t(1|2|16|256)};
  const aurora::gl::MeshLayout reference{32,{{0,0,3,F::F32},{12,4,2,F::F32}}};
  require(aurora_begin_native_frame(),"indexed palette frame admission failed");
  aurora::gl::bind_framebuffer(aurora::gpu::g_frameBuffer.texture->gl(),0);
  aurora::gl::set_viewport(0,0,64,64,0,1);aurora::gl::set_scissor_enabled(false);
  aurora::gl::RasterState raster{};raster.cull=false;raster.depthTest=false;raster.blend=false;
  aurora::gl::apply_raster_state(raster);
  auto texture=aurora::gl::create_texture(4,1,1,aurora::gl::Format::RGBA8,"indexed palette probe");
  const uint8_t texels[]{255,0,0,255,0,255,0,255,255,0,0,255,0,255,0,255};
  aurora::gl::write_texture(texture,0,0,0,4,1,texels,sizeof texels);
  aurora::gl::bind_texture(0,texture->gl(),aurora::gl::sampler(aurora::gl::NearestClamp));
  const auto populate=[&](const aurora::gx::ShaderInfo& info,bool palette) {
    AuroraNativeUniforms uniforms{};
    require(aurora_allocate_native_uniforms(info.uniformSize,&uniforms),"indexed palette uniform allocation failed");
    std::memset(uniforms.data,0,uniforms.size);auto* words=static_cast<float*>(uniforms.data);
    words[2]=words[3]=words[4]=words[5]=64;
    const float projection[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};std::memcpy(words+20,projection,64);
    unsigned offset=36;
    for(unsigned i=0;i<info.matrixLayout.postexCount;++i) {
      const unsigned slot=info.matrixLayout.postexSlots[i];
      const auto& matrix=palette?matrices[slot]:identity;
      std::memcpy(words+offset,matrix.data(),48);offset+=12;
    }
    for(unsigned i=0;i<info.matrixLayout.nrmCount;++i) {std::memcpy(words+offset,identity.data(),48);offset+=12;}
    if(info.usesPTTexMtx.any()) std::memcpy(words+offset,post.data(),48);
    return uniforms;
  };
  const auto& target=aurora::gpu::g_frameBuffer;
  std::vector<uint8_t> actual(size_t(target.width)*target.height*4),expected(actual.size());
  for(unsigned pass=0;pass<3;++pass) {
    const unsigned coordinate=pass==0?0:pass==1?1:7;
    aurora::gx::ShaderConfig config;config.attrs[GX_VA_POS]={GX_DIRECT,3,GX_F32};
    config.attrs[GX_VA_TEX0]={GX_DIRECT,2,GX_F32};config.attrs[GX_VA_PNMTXIDX]={GX_DIRECT,1,GX_U8};
    for(unsigned i:{0u,3u,7u}) config.attrs[GX_VA_TEX0MTXIDX+i]={GX_DIRECT,1,GX_U8};
    config.numTexGens=8;config.dualTexEnabled=true;
    auto& gen=config.tcgs[coordinate];gen.type=GX_TG_MTX2x4;gen.src=GX_TG_TEX0;
    gen.mtx=GX_TEXMTX1;gen.postMtx=GX_PTTEXMTX0;
    config.tevStageCount=1;auto& tev=config.tevStages[0];tev.texMapId=GX_TEXMAP0;tev.texCoordId=GXTexCoordID(coordinate);
    tev.colorPass={GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO,GX_CC_TEXC};
    tev.alphaPass={GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,GX_CA_TEXA};
    const auto info=aurora::gx::build_shader_info(config);
    auto desc=aurora::gx::build_shader_description(config,true,1,true,&indexed);
    desc.origin="tests/native_mesh_buffer_check.cpp:indexed_palette_probe coordinate="+std::to_string(coordinate);
    aurora::gl::Program program(std::move(desc));const auto uniforms=populate(info,true);
    const float clear[]{0,0,1,1};aurora::gl::clear_color(clear);
    aurora::gl::draw_native_mesh(buffer,indexed,0,6,768,6,program.get(),uniforms);
    aurora::gl::read_pixels(target.texture->gl(),0,0,target.width,target.height,actual.data());
    config.attrs[GX_VA_PNMTXIDX]={};for(unsigned i:{0u,3u,7u}) config.attrs[GX_VA_TEX0MTXIDX+i]={};
    config.dualTexEnabled=false;gen.mtx=GX_IDENTITY;gen.postMtx=GX_PTIDENTITY;
    const auto referenceInfo=aurora::gx::build_shader_info(config);
    aurora::gl::Program referenceProgram(aurora::gx::build_shader_description(config,true,1,true,&reference));
    const auto referenceUniforms=populate(referenceInfo,false);aurora::gl::clear_color(clear);
    aurora::gl::draw_native_mesh(buffer,reference,192*(pass+1),6,768,6,referenceProgram.get(),referenceUniforms);
    aurora::gl::read_pixels(target.texture->gl(),0,0,target.width,target.height,expected.data());
    require(actual==expected,"indexed PN/texture/post palette pixels differ from immutable reference");
    bool red=false,green=false;
    for(size_t i=0;i<actual.size();i+=4) {red|=actual[i]>250&&actual[i+1]<2;green|=actual[i]<2&&actual[i+1]>250;}
    require(red&&green,"indexed palette fixture failed to exercise both texture colors");
  }
  require(!std::memcmp(buffer.data(),payload.data(),payload.size()),"indexed palette draw changed immutable vertices");
  aurora_end_native_frame();std::puts("Indexed palette passed: raw U8 groups, PN0/9, TEX0/9, identity20, tex7, inherited selector, post matrices; identical framebuffers");
}
int main(int argc, char** argv) {
  try {
    const auto directory = std::filesystem::temp_directory_path() / "mh3-native-mesh-buffer";
    std::filesystem::create_directories(directory);
    const std::string cache = directory.string(), path = (directory / "payload.bin").string();
    std::array<uint8_t, 512> source{};
    for (size_t i=0; i<source.size(); ++i) source[i]=uint8_t(i*73+11);
    FILE* file=std::fopen(path.c_str(), "wb"); require(file, "cannot create fixture");
    const auto written=std::fwrite(source.data(), 1, source.size(), file); std::fclose(file);
    require(written==source.size(), "fixture write failed");
    AuroraConfig config{}; config.appName="Native mesh storage check";
    config.userPath=config.cachePath=cache.c_str(); config.desiredBackend=BACKEND_OPENGLES;
    config.windowWidth=config.windowHeight=64; config.msaa=1;
    require(aurora_configure_native_target(64,64), "target configuration failed");
    require(aurora_initialize(argc,argv,&config).initializationStatus==AURORA_INITIALIZATION_SUCCESS,
            "renderer initialization failed");
    struct Shutdown { ~Shutdown() { aurora_shutdown(); } } shutdown;
    indexed_palette_probe(directory/"indexed-palettes.bin");
    GLuint sentinel=0; glGenBuffers(1,&sentinel); glBindBuffer(GL_COPY_WRITE_BUFFER,sentinel);
    {
      aurora::gl::NativeMeshBuffer buffer(path.c_str(),64,384);
      require(buffer.size()==384 && !std::memcmp(buffer.data(),source.data()+64,384), "CPU range differs");
      GLint binding=0; glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING,&binding);
      require(GLuint(binding)==sentinel, "mesh load changed buffer binding");
      glBindBuffer(GL_COPY_READ_BUFFER,buffer.get());
      const void* gpu=glMapBufferRange(GL_COPY_READ_BUFFER,0,384,GL_MAP_READ_BIT);
      require(gpu && !std::memcmp(gpu,source.data()+64,384), "GPU payload differs");
      require(glUnmapBuffer(GL_COPY_READ_BUFFER)==GL_TRUE,"GPU mapping failed");
      bool failed=false;
      try { aurora::gl::NativeMeshBuffer truncated(path.c_str(),500,32); }
      catch (const std::runtime_error&) { failed=true; }
      require(failed,"truncated payload accepted");
      require(glIsBuffer(buffer.get()),"failed load invalidated existing payload");
    }
    // Two resident layouts share identical shader sources, but differ in
    // stride, offsets and color storage. Draws must not alias layout state.
    std::array<uint8_t,256> meshes{};
    const float positions[3][3]{{-1,-1,0},{3,-1,0},{-1,3,0}};
    for (unsigned i=0;i<3;++i) {
      std::memcpy(meshes.data()+i*16,positions[i],12);
      const uint8_t red[4]{255,0,0,255};
      std::memcpy(meshes.data()+i*16+12,red,4);
      std::memcpy(meshes.data()+64+i*32+4,positions[i],12);
      const float green[4]{0,1,0,1};
      std::memcpy(meshes.data()+64+i*32+16,green,16);
    }
    const uint16_t indices[]{0,1,2};
    std::memcpy(meshes.data()+224,indices,sizeof(indices));
    file=std::fopen(path.c_str(),"wb");require(file,"cannot write mesh fixture");
    const auto meshWritten=std::fwrite(meshes.data(),1,meshes.size(),file);std::fclose(file);
    require(meshWritten==meshes.size(),"mesh fixture write failed");
    {
      aurora::gl::NativeMeshBuffer buffer(path.c_str(),0,meshes.size());
      using F=aurora::gl::MeshAttributeFormat;
      const aurora::gl::MeshLayout layouts[]{
        {16,{{0,0,3,F::F32},{12,2,4,F::U8N}}},
        {32,{{4,0,3,F::F32},{16,2,4,F::F32}}}};
      aurora::gl::ProgramDesc desc;
      desc.label="resident mesh layout probe";
      desc.vertex=R"(#version 300 es
precision highp float;
layout(location=0) in vec3 mesh_position;
layout(location=2) in vec4 mesh_color0;
layout(std140) uniform MeshUniforms { vec4 tint; };
out vec4 color;
void main(){gl_Position=vec4(mesh_position,1.0);color=mesh_color0*tint;}
)";
      desc.fragment=R"(#version 300 es
precision highp float;
in vec4 color;
layout(location=0) out vec4 output_color;
void main(){output_color=color;}
)";
      desc.blocks.emplace_back("MeshUniforms",0);
      desc.nativeMeshLayout=layouts[0];aurora::gl::Program red(desc);
      desc.nativeMeshLayout=layouts[1];aurora::gl::Program green(desc);
      aurora::gx::ShaderConfig modelConfig;
      modelConfig.attrs[GX_VA_POS]={GX_DIRECT,3,GX_F32};
      modelConfig.attrs[GX_VA_CLR0]={GX_DIRECT,4,GX_RGBA8};
      modelConfig.colorChannels[0].matSrc=GX_SRC_VTX;
      modelConfig.colorChannels[GX_ALPHA0].matSrc=GX_SRC_VTX;
      modelConfig.tevStageCount=1;
      modelConfig.tevStages[0].channelId=GX_COLOR0A0;
      modelConfig.tevStages[0].colorPass={GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO,GX_CC_RASC};
      modelConfig.tevStages[0].alphaPass={GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,GX_CA_RASA};
      const auto modelInfo=aurora::gx::build_shader_info(modelConfig);
      aurora::gl::Program modelRed(aurora::gx::build_shader_description(modelConfig,true,0,true,&layouts[0]));
      aurora::gl::Program modelGreen(aurora::gx::build_shader_description(modelConfig,true,0,true,&layouts[1]));
      auto textureConfig=modelConfig;
      textureConfig.numTexGens=1;textureConfig.dualTexEnabled=true;
      textureConfig.tcgs[0].type=GX_TG_MTX3x4;textureConfig.tcgs[0].src=GX_TG_POS;
      textureConfig.tcgs[0].mtx=GX_TEXMTX0;textureConfig.tcgs[0].postMtx=GX_PTTEXMTX0;
      textureConfig.tevStages[0].texMapId=GX_TEXMAP0;textureConfig.tevStages[0].texCoordId=GX_TEXCOORD0;
      textureConfig.tevStages[0].colorPass={GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO,GX_CC_TEXC};
      textureConfig.tevStages[0].alphaPass={GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,GX_CA_TEXA};
      const auto textureInfo=aurora::gx::build_shader_info(textureConfig);
      aurora::gl::Program textureProgram(aurora::gx::build_shader_description(textureConfig,true,1,true,&layouts[0],1));
      aurora::gl::Program textureFallback(aurora::gx::build_shader_description(textureConfig,true,1,true,&layouts[0]));
      auto texture=aurora::gl::create_texture(2,1,1,aurora::gl::Format::RGBA8,"compact texgen probe");
      const uint8_t texels[]{255,0,0,255,0,255,0,255};
      aurora::gl::write_texture(texture,0,0,0,2,1,texels,8);
      unsigned rendered=0;
      for(unsigned frame=0;frame<12;++frame) {
        aurora_update();
        if(aurora_begin_native_frame()) {
          ++rendered;
          aurora::gl::bind_framebuffer(aurora::gpu::g_frameBuffer.texture->gl(),0);
          aurora::gl::set_viewport(0,0,64,64,0,1);
          aurora::gl::set_scissor_enabled(false);
          aurora::gl::RasterState raster{};raster.cull=false;raster.depthTest=false;
          raster.blend=false;raster.colorMask={true,true,true,true};
          aurora::gl::apply_raster_state(raster);
          AuroraNativeUniforms uniforms{};
          require(aurora_allocate_native_uniforms(16,&uniforms),"uniform allocation failed");
          const float tint[]{1,1,1,1};std::memcpy(uniforms.data,tint,16);
          for(unsigned pass=0;pass<4;++pass) {
            const unsigned which=pass%2;
            aurora::gl::draw_native_mesh(buffer,layouts[which],which?64:0,3,224,3,
                                        which?green.get():red.get(),uniforms);
            std::array<uint8_t,4> pixel{};aurora::gl::read_pixels(aurora::gpu::g_frameBuffer.texture->gl(),32,32,1,1,pixel.data());
            const std::array<uint8_t,4> expected=which?std::array<uint8_t,4>{0,255,0,255}:
                                                        std::array<uint8_t,4>{255,0,0,255};
            require(pixel==expected,"resident layout draw differs");
          }
          AuroraNativeUniforms modelUniforms{};
          require(aurora_allocate_native_uniforms(modelInfo.uniformSize,&modelUniforms),"model uniform allocation failed");
          std::memset(modelUniforms.data,0,modelUniforms.size);
          auto* words=static_cast<float*>(modelUniforms.data);
          words[2]=words[3]=64;words[4]=words[5]=64;
          const float projection[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
          const float pose[]{.25f,0,0,0,0,.25f,0,0,0,0,1,.5f};
          const float normal[]{1,0,0,0,0,1,0,0,0,0,1,0};
          std::memcpy(words+20,projection,64);
          std::memcpy(words+36,pose,48);
          std::memcpy(words+48,normal,48);
          for(unsigned which=0;which<2;++which) {
            const float clear[]{0,0,1,1};aurora::gl::clear_color(clear);
            aurora::gl::draw_native_mesh(buffer,layouts[which],which?64:0,3,224,3,
                                        which?modelGreen.get():modelRed.get(),modelUniforms);
            std::array<uint8_t,4> pixel{};
            aurora::gl::read_pixels(aurora::gpu::g_frameBuffer.texture->gl(),32,32,1,1,pixel.data());
            require(pixel==(which?std::array<uint8_t,4>{0,255,0,255}:std::array<uint8_t,4>{255,0,0,255}),
                    "compact float material shader pixel differs");
            aurora::gl::read_pixels(aurora::gpu::g_frameBuffer.texture->gl(),60,32,1,1,pixel.data());
            require(pixel==std::array<uint8_t,4>{0,0,255,255},"compact pose matrix did not limit triangle coverage");
          }
          AuroraNativeUniforms textureUniforms{};
          require(aurora_allocate_native_uniforms(textureInfo.uniformSize,&textureUniforms),"texgen uniforms allocation failed");
          std::memset(textureUniforms.data,0,textureUniforms.size);
          auto* textureWords=static_cast<float*>(textureUniforms.data);
          textureWords[2]=textureWords[3]=64;
          std::memcpy(textureWords+20,projection,64);
          const auto& matrixLayout=textureInfo.matrixLayout;
          require(matrixLayout.postexCount==2 && matrixLayout.nrmCount==1,"unexpected compact test matrix layout");
          std::memcpy(textureWords+36,pose,48);
          std::memcpy(textureWords+36+12*matrixLayout.postexCount,normal,48);
          const unsigned postOffset=36+12*(matrixLayout.postexCount+matrixLayout.nrmCount);
          const float post[]{2,0,0,0,0,2,0,0,0,0,2,0};
          std::memcpy(textureWords+postOffset,post,48);
          const unsigned dimensionsOffset=postOffset+12*aurora::gx::MaxPTTexMtx;
          const float dimensions[]{2,1,0,0,2,1,0,0};
          std::memcpy(textureWords+dimensionsOffset,dimensions,sizeof(dimensions));
          aurora::gl::bind_texture(0,texture->gl(),aurora::gl::sampler(aurora::gl::NearestClamp));
          for(unsigned projective=0;projective<2;++projective) for(unsigned which=0;which<2;++which) {
            // Fast path has varying Q with a positive lower bound. The
            // uncertified path separately retains the original Q=0 behavior.
            const float mainTexture[]{0,0,0,which?.75f:.25f,0,0,0,.5f,projective?.25f:0,0,0,projective?1.f:0};
            std::memcpy(textureWords+36+12*matrixLayout.postexRemap[10],mainTexture,48);
            aurora::gl::draw_native_mesh(buffer,layouts[0],0,3,224,3,projective?textureProgram.get():textureFallback.get(),textureUniforms);
            std::array<uint8_t,4> pixel{};
            aurora::gl::read_pixels(aurora::gpu::g_frameBuffer.texture->gl(),32,32,1,1,pixel.data());
            require(pixel==(which?std::array<uint8_t,4>{0,255,0,255}:std::array<uint8_t,4>{255,0,0,255}),
                    "compact main/post/projective texgen pixel differs");
          }
          bool rejected=false;
          try {aurora::gl::draw_native_mesh(buffer,layouts[0],0,3,254,3,red.get(),uniforms);}
          catch(const std::runtime_error&){rejected=true;}
          require(rejected,"out of bounds index range accepted");
        }
        aurora_end_native_frame();
      }
      require(rendered>0,"no resident mesh frame admitted");
      require(!std::memcmp(buffer.data(),meshes.data(),meshes.size()),"draw changed immutable mesh payload");
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER,0); glBindBuffer(GL_COPY_READ_BUFFER,0);
    glDeleteBuffers(1,&sentinel);
    require(glGetError()==GL_NO_ERROR,"mesh storage GL error");
    std::puts("Native mesh storage passed: file range, CPU/GPU contents, binding preservation, truncated load, resident draws with alternating layouts, GPU pose/projective texgen, and range rejection");
    return 0;
  } catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}
