// Host-only GPU storage check; readback is diagnostic, never a game operation.
#include <aurora/aurora.h>
#include "gl/gl.hpp"
#include "gl/device.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

static void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
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
layout(location=0) in vec3 a_mesh_position;
layout(location=2) in vec4 a_mesh_color0;
layout(std140) uniform MeshUniforms { vec4 tint; };
out vec4 color;
void main(){gl_Position=vec4(a_mesh_position,1.0);color=a_mesh_color0*tint;}
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
    std::puts("Native mesh storage passed: file range, CPU/GPU contents, binding preservation, truncated load, resident draws with alternating layouts and range rejection");
    return 0;
  } catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}
