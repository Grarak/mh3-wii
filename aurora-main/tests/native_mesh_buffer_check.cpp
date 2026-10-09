// Host-only GPU storage check; readback is diagnostic, never a game operation.
#include <aurora/aurora.h>
#include "gl/gl.hpp"
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
    glBindBuffer(GL_COPY_WRITE_BUFFER,0); glBindBuffer(GL_COPY_READ_BUFFER,0);
    glDeleteBuffers(1,&sentinel);
    require(glGetError()==GL_NO_ERROR,"mesh storage GL error");
    std::puts("Native mesh storage passed: file range, CPU/GPU contents, binding preservation, truncated load");
    return 0;
  } catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}
