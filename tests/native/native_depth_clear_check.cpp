// ROM-free pixel/state proof for the real full-target depth clear facade.
#include <aurora/aurora.h>
#include <aurora/native_geometry.h>
#include "gl/gl.hpp"
#include "gl/device.hpp"
#include "native_graphics.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

static bool suppressDrawing=false;
bool NativeGraphicsFrameShouldRender() {return !suppressDrawing && aurora_native_frame_should_render();}
namespace {
void require(bool value,const char *reason) {if(!value) throw std::runtime_error(reason);}
void pixel(unsigned x,unsigned y,const std::array<uint8_t,4>& expected) {
    uint8_t actual[4]{};
    aurora::gl::read_pixels(aurora::gpu::g_frameBuffer.texture->gl(),x,y,1,1,actual);
    for(unsigned channel=0;channel<4;++channel) if(std::abs(int(actual[channel])-int(expected[channel]))>1) {
        std::fprintf(stderr,"Pixel %u,%u: %u %u %u %u expected %u %u %u %u\n",x,y,
            actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);
        throw std::runtime_error("Native depth clear changed color/alpha or retained stale depth");
    }
}
void quad(const std::array<uint8_t,4>& color) {
    AuroraNativeGeometry geometry{};
    require(aurora_allocate_native_geometry(4,6,&geometry),"Depth clear probe geometry unavailable");
    const float positions[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
    for(unsigned i=0;i<4;++i) {
        auto& vertex=geometry.vertices[i];vertex.position[0]=positions[i][0];vertex.position[1]=positions[i][1];
        vertex.position[2]=0;vertex.position[3]=1;vertex.uv[0]=vertex.uv[1]=0;
        std::memcpy(vertex.color,color.data(),4);
    }
    const uint16_t indices[6]={0,1,2,0,2,3};std::memcpy(geometry.indices,indices,sizeof(indices));
    aurora_submit_native_geometry(&geometry,0);
}
void same(const aurora::gl::OffscreenState& before,const aurora::gl::OffscreenState& after) {
    require(before.viewport==after.viewport && before.depthRange==after.depthRange && before.scissor==after.scissor &&
        before.scissorEnabled==after.scissorEnabled && before.raster.depthWrite==after.raster.depthWrite &&
        before.raster.depthTest==after.raster.depthTest && before.raster.depthFunc==after.raster.depthFunc &&
        before.raster.colorMask==after.raster.colorMask && before.raster.blend==after.raster.blend &&
        before.raster.srcColor==after.raster.srcColor && before.raster.dstColor==after.raster.dstColor &&
        before.raster.srcAlpha==after.raster.srcAlpha && before.raster.dstAlpha==after.raster.dstAlpha &&
        before.framebuffer==after.framebuffer && before.program==after.program && before.vertexArray==after.vertexArray &&
        before.arrayBuffer==after.arrayBuffer && before.textures==after.textures && before.samplers==after.samplers &&
        before.activeTexture==after.activeTexture,"Native full-target depth clear changed retained backend state");
}
}
int main(int argc,char **argv) {
    try {
        const std::string cache=argc>1?argv[1]:"/tmp/mh3-native-depth-clear";
        std::filesystem::create_directories(cache);
        require(aurora_configure_native_target(64,64),"Depth clear target configuration failed");
        AuroraConfig config{};config.appName="MH3 native depth clear check";config.userPath=config.cachePath=cache.c_str();
        config.desiredBackend=BACKEND_OPENGLES;config.windowWidth=config.windowHeight=64;config.msaa=1;
        require(aurora_initialize(argc,argv,&config).initializationStatus==AURORA_INITIALIZATION_SUCCESS,"Depth clear renderer startup failed");
        struct Shutdown {~Shutdown(){aurora_shutdown();}} shutdown;
        aurora_update();require(aurora_begin_native_frame(),"Depth clear probe frame was skipped");
        aurora::gl::bind_framebuffer(aurora::gpu::g_frameBuffer.texture->gl(),aurora::gpu::g_depthBuffer.texture->gl());
        aurora::gl::set_scissor_enabled(false);aurora::gl::set_depth_write(true);aurora::gl::set_color_mask(true,true,true,true);
        const float color[4]={.2f,.4f,.6f,.25f};aurora::gl::clear_color(color);aurora::gl::clear_depth(.8f);
        SetNativeViewport(8,8,32,32,.1f,.9f);SetNativeScissor(8,12,16,20);
        const mh3_raster_state raster{0,1,0,3,1,1,0,0,1,0};SetNativeRaster(raster);
        const mh3_draw_state logical=GetNativeDrawState();
        const auto before=aurora::gl::save_offscreen_state();ClearNativeDepthTarget();
        // SDL can enforce a larger physical window than the 64x64 authored
        // target. Sample the actual scaled scissor rather than fixed pixels.
        const unsigned insideX=unsigned(before.scissor[0]+before.scissor[2]/2);
        const unsigned insideY=unsigned(before.scissor[1]+before.scissor[3]/2);
        same(before,aurora::gl::save_offscreen_state());
        require(std::memcmp(&logical,&GetNativeDrawState(),sizeof(logical))==0,"Depth clear mutated typed application draw state");
        pixel(2,2,{51,102,153,64});pixel(insideX,insideY,{51,102,153,64});
        // The retained scissor, viewport, depth test/write and alpha mask must
        // still apply to a draw issued immediately after the utility clear.
        quad({0,255,0,255});pixel(insideX,insideY,{0,255,0,64});pixel(2,2,{51,102,153,64});
        // Probe depth outside both the old viewport and scissor. A .5 draw
        // passes only if the original .8 depth was cleared over the full target.
        aurora::gl::set_viewport(0,0,aurora::gpu::g_frameBuffer.width,aurora::gpu::g_frameBuffer.height,0,1);
        aurora::gl::set_scissor_enabled(false);
        aurora::gl::RasterState probe;probe.depthTest=true;probe.depthWrite=false;probe.depthFunc=GL_GREATER;
        aurora::gl::apply_raster_state(probe);quad({255,0,0,255});pixel(2,2,{255,0,0,255});pixel(insideX,insideY,{255,0,0,255});
        const auto admitted=aurora::gl::save_offscreen_state();suppressDrawing=true;ClearNativeDepthTarget();
        same(admitted,aurora::gl::save_offscreen_state());pixel(2,2,{255,0,0,255});suppressDrawing=false;
        aurora_end_native_frame();
        std::puts("Native full-target depth clear passed: preserved RGBA/state, cleared outside viewport/scissor, later scissored draw, skipped frame");
        return 0;
    } catch(const std::exception& error) {std::fprintf(stderr,"Native depth clear check failed: %s\n",error.what());return 1;}
}
