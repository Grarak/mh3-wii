// Direct application render state. No GX calls, packets, register banks or FIFO.
#include "native_graphics.h"
#include "gl/gl.hpp"
#include "gl/device.hpp"
#include "dolphin/vi/vi_internal.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {
struct NormalMatrix { std::array<float,9> values; bool known=false; };
std::array<NormalMatrix,10> normalMatrices{};
std::array<int32_t,4> scissor{};
bool viewportKnown=false,scissorKnown=false,rasterKnown=false;
NativeRasterObserver rasterObserver=nullptr;
mh3_draw_state drawState = [] {
    mh3_draw_state s{};
    mh3_draw_identity_position(s.position);
    s.projection[0]=s.projection[5]=s.projection[10]=s.projection[15]=1;
    s.viewport[2]=640;s.viewport[3]=448;s.viewport[5]=1;
    s.raster={0,1,0,3,1,3,1,0,1,1};
    return s;
}();
void RequireOwner() {
    if (!aurora::gl::on_render_thread())
        throw std::runtime_error("Native render state changed outside context owner");
}
std::array<float,2> TargetScale() {
    const auto size=aurora::vi::configured_fb_size();
    if (!size.x || !size.y) throw std::runtime_error("Native target has no base dimensions");
    return {float(aurora::gpu::g_frameBuffer.width)/size.x,
            float(aurora::gpu::g_frameBuffer.height)/size.y};
}
}
const mh3_draw_state& GetNativeDrawState() { return drawState; }
void SetNativeRasterObserver(NativeRasterObserver observer) {
    RequireOwner();rasterObserver=observer;
}
void SetNativeProjection(const float matrix[16],bool perspective) {
    RequireOwner();
    std::memcpy(drawState.projection,matrix,sizeof(drawState.projection));
    drawState.perspective=perspective;
}
void SetNativePositionMatrix(const float matrix[12]) {
    RequireOwner();
    std::memcpy(drawState.position,matrix,sizeof(drawState.position));
}
void SetNativeNormalMatrix(uint32_t selector,const float matrix[9]) {
    RequireOwner();
    if(selector%3 || selector/3>=normalMatrices.size())
        throw std::runtime_error("Invalid native normal-matrix selector");
    for(unsigned i=0;i<9;++i) if(!std::isfinite(matrix[i]))
        throw std::runtime_error("Non-finite native normal matrix");
    auto& destination=normalMatrices[selector/3];
    std::memcpy(destination.values.data(),matrix,sizeof(float)*9);
    destination.known=true;
}
bool GetNativeNormalMatrix(uint32_t selector,float matrix[9]) {
    RequireOwner();
    if(selector%3 || selector/3>=normalMatrices.size())
        throw std::runtime_error("Invalid native normal-matrix selector");
    const auto& source=normalMatrices[selector/3];
    if(!source.known) return false;
    std::memcpy(matrix,source.values.data(),sizeof(float)*9);
    return true;
}
void SetNativeViewport(float x,float y,float width,float height,float near_z,float far_z) {
    RequireOwner();
    const float values[]{x,y,width,height,near_z,far_z};
    for(float v:values) if(!std::isfinite(v)) throw std::runtime_error("Non-finite native viewport");
    if(width<0 || height<0) throw std::runtime_error("Negative native viewport");
    std::memcpy(drawState.viewport,values,sizeof(values));
    viewportKnown=true;
    if (!NativeGraphicsFrameShouldRender()) return;
    const auto scale=TargetScale();
    aurora::gl::set_viewport(std::lround(x*scale[0]),std::lround(y*scale[1]),
        std::lround(width*scale[0]),std::lround(height*scale[1]),
        1.f-std::clamp(std::max(near_z,far_z),0.f,1.f),
        1.f-std::clamp(std::min(near_z,far_z),0.f,1.f));
}
bool GetNativeScissor(int32_t out[4]) {
    RequireOwner();
    if(!out) throw std::runtime_error("Null native scissor destination");
    if(!scissorKnown) return false;
    std::memcpy(out,scissor.data(),sizeof(int32_t)*4);
    return true;
}
void SetNativeScissor(int32_t x,int32_t y,int32_t width,int32_t height) {
    RequireOwner();
    if(width<0 || height<0) throw std::runtime_error("Negative native scissor");
    scissor={x,y,width,height};scissorKnown=true;
    if(!NativeGraphicsFrameShouldRender()) return;
    const auto scale=TargetScale();
    const int32_t left=std::floor(x*scale[0]),top=std::floor(y*scale[1]);
    const int32_t right=std::ceil((double(x)+width)*scale[0]),bottom=std::ceil((double(y)+height)*scale[1]);
    aurora::gl::set_scissor_enabled(true);
    aurora::gl::set_scissor(left,top,right-left,bottom-top);
}
static void SetRaster(const mh3_raster_state& raster,bool publish) {
    RequireOwner();
    if(raster.blend_mode>3 || raster.src_factor>7 || raster.dst_factor>7 ||
       raster.depth_compare>7 || raster.cull_mode>3 || raster.logic_op>15)
        throw std::runtime_error("Native raster state is outside its typed domain");
    aurora::gl::RasterState out;
    // Source color factors read destination color; destination factors read
    // source color. Alpha uses the corresponding alpha factor on each side.
    constexpr GLenum source[]{GL_ZERO,GL_ONE,GL_DST_COLOR,GL_ONE_MINUS_DST_COLOR,
        GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA};
    constexpr GLenum destination[]{GL_ZERO,GL_ONE,GL_SRC_COLOR,GL_ONE_MINUS_SRC_COLOR,
        GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA};
    constexpr GLenum sourceAlpha[]{GL_ZERO,GL_ONE,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA,
        GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA};
    constexpr GLenum destinationAlpha[]{GL_ZERO,GL_ONE,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,
        GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA};
    constexpr GLenum depth[]{GL_NEVER,GL_GREATER,GL_EQUAL,GL_GEQUAL,GL_LESS,GL_NOTEQUAL,GL_LEQUAL,GL_ALWAYS};
    out.blend=raster.blend_mode==1 || raster.blend_mode==3;
    out.srcColor=source[raster.src_factor];out.dstColor=destination[raster.dst_factor];
    out.srcAlpha=sourceAlpha[raster.src_factor];out.dstAlpha=destinationAlpha[raster.dst_factor];
    if(raster.blend_mode==3) {
        out.srcColor=out.dstColor=out.srcAlpha=out.dstAlpha=GL_ONE;
        out.colorEquation=out.alphaEquation=GL_FUNC_REVERSE_SUBTRACT;
    }
    out.depthTest=raster.depth_test!=0;out.depthWrite=raster.depth_write!=0;
    out.depthFunc=depth[raster.depth_compare];
    out.cull=raster.cull_mode!=0;out.cullFace=raster.cull_mode==1 ? GL_FRONT:GL_BACK;
    out.colorMask={raster.color_write!=0,raster.color_write!=0,raster.color_write!=0,raster.alpha_write!=0};
    if(raster.cull_mode==3) {
        out.cull=false;out.depthTest=true;out.depthFunc=GL_NEVER;out.depthWrite=false;
    }
    if(raster.blend_mode==2 && raster.logic_op!=3) {
        if(raster.logic_op==5) {out.colorMask={false,false,false,false};}
        else throw std::runtime_error("Native bitwise logic operation requires a framebuffer material");
    }
    drawState.raster=raster;
    rasterKnown=true;
    if(publish && rasterObserver) rasterObserver(raster);
    if(NativeGraphicsFrameShouldRender()) aurora::gl::apply_raster_state(out);
}
void SetNativeRaster(const mh3_raster_state& raster) {SetRaster(raster,true);}

void RestoreNativeRenderState() {
    RequireOwner();
    if(!NativeGraphicsFrameShouldRender()) return;
    // Frame clearing and presentation use their own backend state. Reapply
    // the application's last published values, including skipped-frame writes.
    // Convert logical rectangles again because the target may have resized.
    if(viewportKnown) {
        const auto& v=drawState.viewport;
        SetNativeViewport(v[0],v[1],v[2],v[3],v[4],v[5]);
    }
    if(scissorKnown) SetNativeScissor(scissor[0],scissor[1],scissor[2],scissor[3]);
    if(rasterKnown) SetRaster(drawState.raster,false);
}
