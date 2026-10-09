// RMHP08 application UI paths, reconstructed from their Ghidra C listings.
// This is the guest ABI boundary; typed C writes native attributes directly.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "native_graphics.h"
#include "../native/mh3_ui.h"
#include "../native/mh3_system_ui.h"
#include "../native/adapters/model_state_bridge.h"
#include "../native/adapters/ui_bridge.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
constexpr uint32_t Shape=0x8066CDA0,ScreenWidth=0x8065B4C0,ScreenHeight=0x8065B4C4;
uint32_t uiFlags=0;
bool uiTextured=false;
uint32_t uiStages=1;
struct TextureCoordinates { uint32_t type=1,source=4,matrix=60; } uiCoordinates;
uint64_t textureProducerGeneration=0;
NativeUISourceObserver sourceObserver=nullptr;
uint64_t NextGeneration(uint32_t payload) {
    textureProducerGeneration=std::max(textureProducerGeneration,ReadNativeTextureGeneration(payload))+1;
    if(!textureProducerGeneration) throw std::runtime_error("Native UI producer generation overflow");
    return textureProducerGeneration;
}
mh3_native_texture_binding ResourceBinding(uint32_t reference) {
    mh3_native_texture_binding binding{};binding.source=ReadNativeResTexture(reference);
    binding.sampler.min_filter=binding.source.levels>1 ? 5 : 1;binding.sampler.mag_filter=1;
    binding.sampler.edge_lod=1;return binding;
}
void Checked(mh3_res_status status) {
    if(status!=MH3_RES_OK) throw std::runtime_error("Native UI input failed: "+std::to_string(status));
}
mh3_res_span Span(uint32_t address,size_t size) {
    return {Memory::GetPointer(address,size),size};
}
void ReadPosition(uint32_t address,float out[3]) {
    for(unsigned i=0;i<3;++i) out[i]=Memory::ReadFloat32(address+i*4);
}
void ReadColor(uint32_t address,uint8_t out[4]) {
    for(unsigned i=0;i<4;++i) out[i]=Memory::Read8(address+i);
}
void Identity() { float matrix[12];mh3_draw_identity_position(matrix);SetNativePositionMatrix(matrix); }
void Ortho(float top,float bottom,float left,float right,float nearPlane,float farPlane) {
    float matrix[16];
    if(!mh3_draw_ortho(matrix,top,bottom,left,right,nearPlane,farPlane))
        throw std::runtime_error("Native UI has a degenerate orthographic projection");
    SetNativeProjection(matrix,false);
}
void ScreenOrtho(float nearPlane=0,float farPlane=1) {
    Ortho(0,Memory::ReadFloat32(ScreenHeight),0,Memory::ReadFloat32(ScreenWidth),nearPlane,farPlane);
}
void RasterQuad(bool blend,bool depthTest) {
    auto raster=GetNativeDrawState().raster;
    if(blend) {raster.blend_mode=1;raster.src_factor=4;raster.dst_factor=5;raster.logic_op=0;}
    raster.color_write=1;raster.cull_mode=0;raster.depth_test=depthTest;raster.depth_compare=7;raster.depth_write=0;
    SetNativeRaster(raster);SetNativeDepthCompareBeforeTexture(false);
}
void Invalidate(GuestCall& call,uint32_t fields=0x7FF) { call(0x800885F4,{fields}); }
void RestoreWorld(GuestCall& call) {
    uiStages=1;call(0x800E47B8);Invalidate(call);
}
bool Allocate(mh3_ui_primitive primitive,uint32_t count,AuroraNativeGeometry& geometry) {
    uint32_t vertices,indices;
    Checked(mh3_ui_geometry_counts(primitive,count,&vertices,&indices));
    return indices && aurora_allocate_native_geometry(vertices,indices,&geometry);
}
void DrawVertices(mh3_ui_primitive primitive,const mh3_ui_vertex* vertices,uint32_t count,
                  bool textured,float width=1) {
    if(!NativeGraphicsFrameShouldRender()) return;
    if(uiStages!=1) throw std::runtime_error("Native UI draw requires exactly one color stage");
    if(textured!=uiTextured) throw std::runtime_error("Native UI draw disagrees with its color pipeline");
    AuroraNativeGeometry geometry{};
    if(!Allocate(primitive,count,geometry)) return;
    Checked(mh3_ui_build_vertices(&GetNativeDrawState(),primitive,vertices,count,width,&geometry));
    aurora_submit_native_geometry(&geometry,textured);
}
void SetupPrimitive(GuestCall& call,uint32_t flags,uint32_t texture,uint32_t palette,const float* overrideMatrix=nullptr) {
    uiFlags=flags;
    if(flags&1) {
        call(0x800E47B8);
        GuestFrame scratch(call.cpu,0x40);
        call(0x802BDCA4,{scratch[0x10]}); // get_current_view_mtx, pure CPU copy
        float matrix[12];
        for(unsigned i=0;i<12;++i) matrix[i]=Memory::ReadFloat32(scratch[0x10]+i*4);
        SetNativePositionMatrix(matrix);
    } else {
        ScreenOrtho(-10000,10000);
        if(overrideMatrix) SetNativePositionMatrix(overrideMatrix);else Identity();
    }
    auto raster=GetNativeDrawState().raster;raster.cull_mode=0;SetNativeRaster(raster);
    SetNativeDepthCompareBeforeTexture(false);
    auto material=GetNativeMaterialState();material.channel_count=1;material.channel[0].color=material.channel[0].alpha=(mh3_pack_channel_control){1,1,0,0,2,0};material.channel[0].flags|=MH3_CHANNEL_CONTROL_COLOR|MH3_CHANNEL_CONTROL_ALPHA;
    for(unsigned field=MH3_PIXEL_ALPHA_COMPARE0;field<=MH3_PIXEL_ALPHA_OPERATION;++field){material.pixel.known|=UINT64_C(1)<<field;material.pixel.values[field]=field==MH3_PIXEL_ALPHA_COMPARE0||field==MH3_PIXEL_ALPHA_COMPARE1?7:field==MH3_PIXEL_ALPHA_OPERATION?1:0;}
    mh3_system_ui_application_pipeline(&material,(flags&2)!=0);if(flags&2){const uint32_t stage=0;mh3_system_ui_direct_stages(&material,&stage,1);}SetNativeMaterial(material);
    uiTextured=(flags&2)!=0;uiStages=1;
    if(uiTextured) {
        uiCoordinates={1,4,60};
        auto binding=ResourceBinding(texture);binding.map_id=0;
        binding.sampler.wrap_s=binding.sampler.wrap_t=0;
        if(binding.source.indexed) {
            auto colors=ReadNativeResPalette(palette);BindNativePalette(0,colors);
        }
        StoreNativeTextureObject(0x8066CE98,binding);BindNativeTexture(binding);
    }
}
void ConfigureDirectCoordinates(uint32_t id,uint32_t type,uint32_t source,uint32_t matrix) {
    if(id!=0 || source!=4 || (type!=0 && type!=1) || (matrix!=60 && matrix!=54))
        throw std::runtime_error("Native UI texture coordinates require an unsupported generator");
    uiCoordinates={type,source,matrix};
    auto material=GetNativeMaterialState();material.texgen[0].known|=1u<<MH3_TEXGEN_PROJECTION;material.texgen[0].values[MH3_TEXGEN_PROJECTION]=type==0;SetNativeMaterial(material);
}
mh3_ui_stream ShapeStream(uint32_t flags,uint32_t count) {
    mh3_ui_stream stream{};stream.count=count;
    stream.position_format=flags&1?MH3_UI_POSITION_FLOAT_3:flags&0x10000?MH3_UI_POSITION_FLOAT_2:MH3_UI_POSITION_I16_2;
    stream.positions=Span(Memory::Read32(Shape+8),size_t(count)*(flags&1?12:flags&0x10000?8:4));
    stream.color_stride=flags&0x20000?0:4;
    stream.colors=Span(Memory::Read32(Shape+12),count?size_t(count-1)*stream.color_stride+4:0);
    stream.uv_scale[0]=stream.uv_scale[1]=1;
    if(flags&2) {
        stream.uv_format=flags&0x40000?MH3_UI_UV_I16:MH3_UI_UV_FLOAT;
        stream.uvs=Span(Memory::Read32(Shape+16),size_t(count)*(flags&0x40000?4:8));
        if(flags&0x40000) {
            const auto binding=ResourceBinding(0x8066CE80);
            if(!binding.source.width||!binding.source.height) throw std::runtime_error("Native shape texture has zero dimensions");
            stream.uv_scale[0]=1.f/binding.source.width;stream.uv_scale[1]=1.f/binding.source.height;
        }
        if(flags&8) {
            stream.transform_uv=1;
            for(unsigned i=0;i<12;++i) stream.uv_matrix[i]=Memory::ReadFloat32(0x8066CE44+i*4);
        }
    }
    return stream;
}
void ObserveShape(uint32_t flags,uint32_t count) {
    if(!sourceObserver || !(flags&2) || !count) return;
    if((flags&1) || Memory::Read8(Shape+4)!=MH3_UI_QUADS)
        throw std::runtime_error("Native source observer requires application 2D quads");
    const auto stream=ShapeStream(flags,count);auto state=GetNativeDrawState();
    if(!mh3_draw_ortho(state.projection,0,Memory::ReadFloat32(ScreenHeight),0,Memory::ReadFloat32(ScreenWidth),-10000,10000))
        throw std::runtime_error("Native source observer has invalid screen dimensions");
    state.perspective=0;
    if(flags&4) for(unsigned i=0;i<12;++i) state.position[i]=Memory::ReadFloat32(0x8066CE14+i*4);
    else mh3_draw_identity_position(state.position);
    auto binding=ResourceBinding(0x8066CE80);
    binding.sampler.wrap_s=Memory::Read32(0x8066CE74);binding.sampler.wrap_t=Memory::Read32(0x8066CE78);
    mh3_native_palette_source palette{};const mh3_native_palette_source* colors=nullptr;
    if(binding.source.indexed) {palette=ReadNativeResPalette(0x8066CE84);colors=&palette;}
    sourceObserver(stream,state,binding,colors);
}
void DrawBeta(CpuContext* cpu,bool textured) {
    GuestFrame frame(cpu,0x20);GuestCall call{cpu};
    const auto position=frame.Arg(3),size=frame.Arg(4),color=frame.Arg(5),texture=frame.Arg(6);
    if(!NativeGraphicsFrameShouldRender()) return;
    if(textured) ScreenOrtho();
    else {
        call(0x80040354,{frame[0x10]});
        Ortho(0,Memory::ReadFloat32(frame[0x14]),0,Memory::ReadFloat32(frame[0x10]),0,1);
    }
    Identity();RasterQuad(true,true);uiStages=1;uiTextured=textured;
    if(textured) BindNativeTexture(ReadNativeTextureObject(texture));
    float origin[3];ReadPosition(position,origin);uint8_t rgba[4];ReadColor(color,rgba);
    mh3_ui_vertex vertices[4];
    mh3_ui_rect(vertices,origin[0],origin[1],origin[0]+Memory::ReadFloat32(size),origin[1]+Memory::ReadFloat32(size+4),
                origin[2],rgba,0,0,1,1);
    DrawVertices(MH3_UI_QUADS,vertices,4,textured);
}
} // namespace

void SetNativeUISourceObserver(NativeUISourceObserver observer) {sourceObserver=observer;}

extern "C" void MH3_Setup2DProjection(CpuContext*) {
    Ortho(0,480,0,640,0,1);Identity();
}
REGISTER_NATIVE_FUNCTION_AS(0x80046E98,MH3_Setup2DProjection,"Setup2DProjection");
extern "C" void MH3_UIScreenProjection(CpuContext*) { ScreenOrtho(-10000,10000); }
REGISTER_NATIVE_FUNCTION_AS(0x80052770,MH3_UIScreenProjection,"UIScreenProjection");
extern "C" void MH3_DrawBeta(CpuContext* cpu) { DrawBeta(cpu,false); }
REGISTER_NATIVE_FUNCTION_AS(0x80041470,MH3_DrawBeta,"DrawBeta");
extern "C" void MH3_DrawBetaTexture(CpuContext* cpu) { DrawBeta(cpu,true); }
REGISTER_NATIVE_FUNCTION_AS(0x8005AC3C,MH3_DrawBetaTexture,"DrawBetaTexture");
extern "C" void MH3_DebugDrawSetup(CpuContext* cpu) {
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};
    const auto mode=Memory::Read32(frame.Arg(13)-0x46B8);
    const float width=Memory::Read16(mode+4)*0.913460016f,height=Memory::Read16(mode+6);
    Invalidate(call,4);
    Ortho(-height*0.5f,height*0.5f,-width*0.5f,width*0.5f,-10,10);Identity();
    auto raster=GetNativeDrawState().raster;
    raster.blend_mode=1;raster.src_factor=4;raster.dst_factor=5;raster.logic_op=5;
    raster.alpha_write=0;raster.depth_test=0;raster.depth_compare=7;raster.depth_write=0;raster.cull_mode=2;
    SetNativeRaster(raster);uiTextured=false;uiStages=1;
    auto material=GetNativeMaterialState();mh3_system_ui_fade_material(&material);material.cull_mode=2;const uint32_t stage=0;mh3_system_ui_direct_stages(&material,&stage,1);SetNativeMaterial(material);
    mh3_native_fog fog{};SetNativeFog(fog);
}
REGISTER_NATIVE_FUNCTION_AS(0x8005AF80,MH3_DebugDrawSetup,"DebugDrawSetup");
extern "C" void MH3_UITextureBind(CpuContext* cpu) {
    auto binding=ResourceBinding(cpu->gpr[3]);binding.map_id=cpu->gpr[5];binding.tlut_id=cpu->gpr[5];
    if(binding.map_id>=8) throw std::runtime_error("Native UI texture slot is out of range");
    binding.sampler.wrap_s=cpu->gpr[6];binding.sampler.wrap_t=cpu->gpr[7];
    if(binding.source.indexed) {
        if(!cpu->gpr[4] || !Memory::Read32(cpu->gpr[4])) return; // original optional indexed palette path
        BindNativePalette(binding.tlut_id,ReadNativeResPalette(cpu->gpr[4]));
    }
    StoreNativeTextureObject(0x8066CE98+binding.map_id*32,binding);BindNativeTexture(binding);
}
REGISTER_NATIVE_FUNCTION_AS(0x80052C90,MH3_UITextureBind,"UITextureBind");
extern "C" void MH3_UIColorPipeline(CpuContext*) { uiTextured=false;uiStages=1;auto material=GetNativeMaterialState();mh3_system_ui_application_pipeline(&material,0);SetNativeMaterial(material); }
REGISTER_NATIVE_FUNCTION_AS(0x80052F68,MH3_UIColorPipeline,"UIColorPipeline");
extern "C" void MH3_UITexturePipeline(CpuContext* cpu) {
    const auto texture=cpu->gpr[3],palette=cpu->gpr[4];
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};
    call(0x80052C90,{texture,palette,0,0,0});
    uiTextured=true;uiStages=1;ConfigureDirectCoordinates(0,1,4,60);
    auto material=GetNativeMaterialState();mh3_system_ui_application_pipeline(&material,1);const uint32_t stage=0;mh3_system_ui_direct_stages(&material,&stage,1);SetNativeMaterial(material);
}
REGISTER_NATIVE_FUNCTION_AS(0x80052FB4,MH3_UITexturePipeline,"UITexturePipeline");
extern "C" void MH3_UITextureCoordinates(CpuContext* cpu) {
    ConfigureDirectCoordinates(cpu->gpr[3],cpu->gpr[4],cpu->gpr[5],cpu->gpr[6]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80053028,MH3_UITextureCoordinates,"UITextureCoordinates");
REGISTER_NATIVE_FUNCTION_AS(0x8005AF74,MH3_UITextureCoordinates,"BetaTextureCoordinates");
REGISTER_NATIVE_FUNCTION_AS(0x8005C564,MH3_UITextureCoordinates,"FontTextureCoordinates");
REGISTER_NATIVE_FUNCTION_AS(0x80048D3C,MH3_UITextureCoordinates,"SpriteTextureCoordinates");
extern "C" void MH3_UIStageCount(CpuContext* cpu) {
    const uint32_t count=cpu->gpr[3]&255;if(!count||count>16)throw std::runtime_error("Native UI stage count is outside material storage");uint32_t stages[16];for(unsigned i=0;i<count;++i){stages[i]=Memory::Read32(0x8056fc80+i*4);if(stages[i]>=16)throw std::runtime_error("Native UI direct stage table index invalid");}
    uiStages=count;auto material=GetNativeMaterialState();mh3_system_ui_direct_stages(&material,stages,count);SetNativeMaterial(material);
}
REGISTER_NATIVE_FUNCTION_AS(0x80053034,MH3_UIStageCount,"UIStageCount");
extern "C" void MH3_UIVertexFormat(CpuContext* cpu) { uiFlags=cpu->gpr[3]; }
REGISTER_NATIVE_FUNCTION_AS(0x800530D0,MH3_UIVertexFormat,"UIVertexFormat");
extern "C" void MH3_UIPrimitiveSetup(CpuContext* cpu) {
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};
    SetupPrimitive(call,frame.Arg(3)&255,frame.Arg(4),frame.Arg(5));
}
REGISTER_NATIVE_FUNCTION_AS(0x800531C4,MH3_UIPrimitiveSetup,"UIPrimitiveSetup");
extern "C" void MH3_UIPrimitiveFinish(CpuContext* cpu) {
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};RestoreWorld(call);
}
REGISTER_NATIVE_FUNCTION_AS(0x800533D4,MH3_UIPrimitiveFinish,"UIPrimitiveFinish");
extern "C" void MH3_DrawLine2D(CpuContext* cpu) {
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};
    if(NativeGraphicsFrameShouldRender()) {
        SetupPrimitive(call,0,0,0);mh3_ui_vertex vertices[2]{};
        for(unsigned i=0;i<2;++i) {
            const uint32_t point=frame.Arg(3+i);
            vertices[i].position[0]=int16_t(Memory::Read16(point));vertices[i].position[1]=int16_t(Memory::Read16(point+2));
            ReadColor(point+4,vertices[i].color);
        }
        DrawVertices(MH3_UI_LINES,vertices,2,false,1);
    }
    RestoreWorld(call);
}
REGISTER_NATIVE_FUNCTION_AS(0x8005342C,MH3_DrawLine2D,"DrawLine2D");
extern "C" void MH3_DrawRect2D(CpuContext* cpu) {
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};
    if(NativeGraphicsFrameShouldRender()) {
        SetupPrimitive(call,0,0,0);const auto record=frame.Arg(3);uint8_t rgba[4];ReadColor(record+8,rgba);
        mh3_ui_vertex vertices[4];
        mh3_ui_rect(vertices,int16_t(Memory::Read16(record)),int16_t(Memory::Read16(record+2)),
            int16_t(Memory::Read16(record+4)),int16_t(Memory::Read16(record+6)),0,rgba,0,0,0,0);
        DrawVertices(MH3_UI_QUADS,vertices,4,false);
    }
    RestoreWorld(call);
}
REGISTER_NATIVE_FUNCTION_AS(0x80053580,MH3_DrawRect2D,"DrawRect2D");
extern "C" void MH3_DrawRect2DTexture(CpuContext* cpu) {
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};
    if(NativeGraphicsFrameShouldRender()) {
        const auto record=frame.Arg(3),texture=frame.Arg(4),palette=frame.Arg(5);
        SetupPrimitive(call,2,texture,palette);const auto binding=ResourceBinding(texture);
        if(!binding.source.width || !binding.source.height) throw std::runtime_error("Native UI texture has zero dimensions");
        const float du=1.f/binding.source.width,dv=1.f/binding.source.height;uint8_t rgba[4];ReadColor(record+8,rgba);
        mh3_ui_vertex vertices[4];
        mh3_ui_rect(vertices,int16_t(Memory::Read16(record)),int16_t(Memory::Read16(record+2)),
            int16_t(Memory::Read16(record+4)),int16_t(Memory::Read16(record+6)),0,rgba,
            int16_t(Memory::Read16(record+12))*du,int16_t(Memory::Read16(record+14))*dv,
            int16_t(Memory::Read16(record+16))*du,int16_t(Memory::Read16(record+18))*dv);
        DrawVertices(MH3_UI_QUADS,vertices,4,true);
    }
    RestoreWorld(call);
}
REGISTER_NATIVE_FUNCTION_AS(0x80053620,MH3_DrawRect2DTexture,"DrawRect2DTexture");

extern "C" void MH3_UIShapeSetup(CpuContext* cpu) {
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};
    const auto flags=Memory::Read32(Shape);float matrix[12];const float* overrideMatrix=nullptr;
    if(!(flags&1) && (flags&4)) {
        for(unsigned i=0;i<12;++i) matrix[i]=Memory::ReadFloat32(0x8066CE14+i*4);
        overrideMatrix=matrix;
    }
    SetupPrimitive(call,flags,0x8066CE80,0x8066CE84,overrideMatrix);
    if(flags&2) {
        auto binding=ResourceBinding(0x8066CE80);
        binding.sampler.wrap_s=Memory::Read32(0x8066CE74);binding.sampler.wrap_t=Memory::Read32(0x8066CE78);
        StoreNativeTextureObject(0x8066CE98,binding);BindNativeTexture(binding);
        ConfigureDirectCoordinates(0,flags&8 ? 0 : 1,4,flags&8 ? 54 : 60);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x800545E4,MH3_UIShapeSetup,"UIShapeSetup");
extern "C" void MH3_DrawShape(CpuContext* cpu) {
    const auto flags=Memory::Read32(Shape);
    if((flags&0x3000)!=0x3000) return;
    const auto count=Memory::Read16(Shape+6);ObserveShape(flags,count);
    if(!NativeGraphicsFrameShouldRender()) return;
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};
    call(0x800545E4);
    const auto primitiveValue=Memory::Read8(Shape+4);
    if(!count) {RestoreWorld(call);return;}
    if(primitiveValue>MH3_UI_TRIANGLE_FAN) return; // original unsupported shape selector emits nothing
    const auto primitive=mh3_ui_primitive(primitiveValue);
    if((flags&2) && (primitive==MH3_UI_POINTS || primitive==MH3_UI_LINES || primitive==MH3_UI_LINE_STRIP) &&
       Memory::Read8(0x8066CE7C)!=0)
        throw std::runtime_error("Native UI textured line/point offset requires a pixel offset implementation");
    const auto stream=ShapeStream(flags,count);
    AuroraNativeGeometry geometry{};
    if(Allocate(primitive,count,geometry)) {
        Checked(mh3_ui_build_stream(&GetNativeDrawState(),primitive,&stream,Memory::Read8(Shape+5)/6.f,&geometry));
        aurora_submit_native_geometry(&geometry,(flags&2)!=0);
    }
    RestoreWorld(call);
}
REGISTER_NATIVE_FUNCTION_AS(0x8005480C,MH3_DrawShape,"DrawShape");

extern "C" void MH3_DrawGradientFade(CpuContext* cpu) {
    const auto self=cpu->gpr[3];
    if(!Memory::Read8(self) || !NativeGraphicsFrameShouldRender()) return;
    GuestFrame frame(cpu,0x20);GuestCall call{cpu};call(0x80040354,{frame[0x10]});
    const float width=Memory::ReadFloat32(frame[0x10]),height=Memory::ReadFloat32(frame[0x14]);
    const bool wide=call(0x8004045C)!=0;
    const float split=wide ? Memory::ReadFloat32(ScreenHeight)*Memory::ReadFloat32(0x80798118) : Memory::ReadFloat32(ScreenHeight);
    Ortho(0,height,0,width,0,1);Identity();RasterQuad(true,true);uiStages=1;uiTextured=false;
    auto raster=GetNativeDrawState().raster;
    const auto mode=Memory::Read8(self+1);
    if(mode==1) {raster.blend_mode=1;raster.dst_factor=1;}
    else if(mode==2) raster.blend_mode=3;
    SetNativeRaster(raster);
    auto drawBand=[&](float top,float middle,float bottom,uint32_t colors) {
        mh3_ui_vertex vertices[8]{};
        const float x[8]={0,0,width,width,0,0,width,width};
        const float y[8]={middle,top,top,middle,bottom,middle,middle,bottom};
        const unsigned c[8]={2,0,1,3,4,2,3,5};
        for(unsigned i=0;i<8;++i) {
            vertices[i].position[0]=int16_t(int(x[i]));vertices[i].position[1]=int16_t(int(y[i]));
            ReadColor(colors+c[i]*4,vertices[i].color);
        }
        DrawVertices(MH3_UI_QUADS,vertices,8,false);
    };
    drawBand(0,int16_t(Memory::Read16(self+0x24)),split,self+4);
    if(wide) {
        const auto middle=int16_t(int16_t(int(split))+int16_t(Memory::Read16(self+0x26)));
        drawBand(split,middle,Memory::ReadFloat32(ScreenHeight),self+0x2C);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80057A08,MH3_DrawGradientFade,"DrawGradientFade");

extern "C" void MH3_DrawUISprites(CpuContext* cpu) {
    if(!NativeGraphicsFrameShouldRender()) return;
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};
    const auto list=Memory::Read32(0x80796D30+(frame.Arg(3)&255)*4);
    bool active=false;
    for(unsigned slot=0;slot<32;++slot) if(int16_t(Memory::Read16(list+64+slot*2))>0) {active=true;break;}
    if(!active) return;
    call(0x8005AF80);ScreenOrtho();Identity();RasterQuad(true,true);uiStages=1;uiTextured=true;
    for(unsigned slot=0;slot<32;++slot) {
        const auto textureId=int16_t(Memory::Read16(list+slot*2)),count=int16_t(Memory::Read16(list+64+slot*2));
        if(textureId==-1 || count<=0) continue;
        if(count>16) throw std::runtime_error("Native sprite batch exceeds its CPU quad slots");
        const auto texture=call(0x800D05CC,{uint32_t(textureId)});
        BindNativeTexture(ReadNativeTextureObject(texture));
        const auto records=list+128+slot*576;
        AuroraNativeGeometry geometry{};
        if(!Allocate(MH3_UI_QUADS,uint32_t(count)*4,geometry)) continue;
        for(unsigned quadIndex=0;quadIndex<unsigned(count);++quadIndex) {
            const auto record=records+quadIndex*36;uint8_t rgba[4];ReadColor(record+32,rgba);
            mh3_ui_vertex vertices[4];
            mh3_ui_rect(vertices,int16_t(Memory::Read16(record+4)),int16_t(Memory::Read16(record+8)),
                int16_t(Memory::Read16(record+6)),int16_t(Memory::Read16(record+10)),0,rgba,
                Memory::ReadFloat32(record+16),Memory::ReadFloat32(record+24),Memory::ReadFloat32(record+20),Memory::ReadFloat32(record+28));
            AuroraNativeGeometry quadGeometry{geometry.vertices+quadIndex*4,geometry.indices+quadIndex*6,4,6,geometry.frame,geometry.slot};
            Checked(mh3_ui_build_vertices(&GetNativeDrawState(),MH3_UI_QUADS,vertices,4,1,&quadGeometry));
            for(unsigned i=0;i<6;++i) quadGeometry.indices[i]+=uint16_t(quadIndex*4);
        }
        aurora_submit_native_geometry(&geometry,true);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x800489C8,MH3_DrawUISprites,"DrawUISprites");

static uint32_t BuildUITextureTPL(CpuContext* cpu) {
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};
    const auto tpl=frame.Arg(3),id=frame.Arg(4);
    call(0x804E4C30,{tpl}); // TPLBind relocates CPU pointers; no rendering
    uint32_t record=0;
    if(id==0xFF) record=0x8065B3CC;
    else if(id==0xFE) record=0x8065B404;
    else if(id==0xFD) record=0x8065B43C;
    else if(id<32) record=0x8065AB1C+id*56;
    else return 0;
    const uint32_t count=Memory::Read32(tpl+4);
    uint32_t created=0;
    for(uint32_t index=0;index<count;++index) {
        if(id>=0xFD) { if(Memory::Read8(record)) break; }
        else if(id+index>=32) break;
        if(!Memory::Read8(record)) {
            if(id==5 && index==2) record=0x8065AD84;
            const auto descriptor=call(0x804E4D50,{tpl,index});
            const auto header=Memory::Read32(descriptor);
            mh3_ui_texture_header image{};Checked(mh3_ui_texture_header_read(Span(header,36),&image));
            mh3_native_texture_binding binding{};
            binding.source.resource_address=header;binding.source.payload_address=image.payload_address;
            binding.source.width=image.width;binding.source.height=image.height;binding.source.format=image.format;
            binding.source.levels=1;binding.source.generation=NextGeneration(image.payload_address);
            binding.source.mutable_source=id==0xFE;
            binding.sampler.wrap_s=image.wrap_s;binding.sampler.wrap_t=image.wrap_t;
            binding.sampler.min_filter=binding.sampler.mag_filter=1;
            Memory::Write32(record+0x24,image.payload_address);Memory::Write16(record+0x28,uint16_t(image.width));
            Memory::Write16(record+0x2A,uint16_t(image.height));Memory::Write32(record+0x2C,image.format);
            Memory::Write32(record+0x30,image.wrap_s);Memory::Write32(record+0x34,image.wrap_t);
            MarkNativeTextureProduced(image.payload_address,binding.source.generation);
            StoreNativeTextureObject(record+4,binding);Memory::Write8(record,1);++created;
            if(id==0xFF) break;
        }
        record+=56;
    }
    return created;
}
extern "C" void MH3_UITextureTPL(CpuContext* cpu) { cpu->gpr[3]=BuildUITextureTPL(cpu); }
REGISTER_NATIVE_FUNCTION_AS(0x800D0174,MH3_UITextureTPL,"UITextureTPL");

// Explicit producer hook for any CPU code replacing font atlas pixels. Font
// lifecycle construction above publishes the initial generation. The RMHP08
// glyph registration helper8005bb54 returns without modifying atlas pixels.
void NativeUIFontAtlasProduced(uint32_t payload) {
    MarkNativeTextureProduced(payload,NextGeneration(payload));
}

static uint32_t ProduceFontAtlas(CpuContext* cpu) {
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};
    const auto destination=frame.Arg(3);
    const auto bytes=call(0x80041050,{0x8057D690,destination}); // verified pure CPU word-stream decoder
    if(bytes<64 || Memory::Read32(destination)!=0x0020AF30 || Memory::Read32(destination+4)!=1)
        throw std::runtime_error("Native font producer did not decode its single-image TPL");
    const auto table=Memory::Read32(destination+8);
    if(table>bytes-8) throw std::runtime_error("Native font producer table exceeds its decoded bytes");
    const auto header=Memory::Read32(destination+table);
    if(header>bytes-36) throw std::runtime_error("Native font producer header exceeds its decoded bytes");
    const auto payload=Memory::Read32(destination+header+8);
    if(payload>=bytes) throw std::runtime_error("Native font producer image exceeds its decoded bytes");
    NativeUIFontAtlasProduced(destination+payload);
    return bytes;
}
extern "C" void MH3_FontAtlasDecode(CpuContext* cpu) { cpu->gpr[3]=ProduceFontAtlas(cpu); }
REGISTER_NATIVE_FUNCTION_AS(0x8004119C,MH3_FontAtlasDecode,"FontAtlasDecode");

extern "C" void MH3_UIShapeTextureObject(CpuContext* cpu) {
    GuestFrame frame(cpu,0x20);GuestCall call{cpu};
    const auto id=frame.Arg(3)&0xFFFF;
    if(id>=33) throw std::runtime_error("Native shape texture resource id is out of range");
    call(0x80053A18,{id});
    Memory::Write32(frame[0x10],Memory::Read32(0x8066CFF8+id*4));
    Memory::Write32(frame[0x14],Memory::Read32(0x8066D07C+id*4));
    Memory::Write32(frame.Arg(4),0x8066CE98);Memory::Write32(frame.Arg(5),0x8066CF98);
    Memory::Write32(0x8066CE74,0);Memory::Write32(0x8066CE78,0);
    auto binding=ResourceBinding(frame[0x10]);binding.map_id=0;binding.tlut_id=0;
    binding.sampler.wrap_s=binding.sampler.wrap_t=0;
    if(binding.source.indexed) {
        if(!Memory::Read32(frame[0x14])) {Memory::Write32(frame.Arg(5),0);return;}
        const auto palette=ReadNativeResPalette(frame[0x14]);
        StoreNativePaletteObject(0x8066CF98,palette);
        BindNativePalette(0,palette);
    } else Memory::Write32(frame.Arg(5),0);
    StoreNativeTextureObject(0x8066CE98,binding);
}
REGISTER_NATIVE_FUNCTION_AS(0x80054CD8,MH3_UIShapeTextureObject,"UIShapeTextureObject");

extern "C" void MH3_FontFlush(CpuContext* cpu) {
    const auto font=Memory::Read32(0x80796D98);
    if(!font) return;
    const auto queued=Memory::Read32(font+0x1505C),flushed=Memory::Read32(font+0x168CC);
    if(!queued || queued==flushed) return;
    const int16_t end[2]={int16_t(Memory::Read16(font+0x15094)),int16_t(Memory::Read16(font+0x15096))};
    const int16_t begin[2]={int16_t(Memory::Read16(font+0x168C8)),int16_t(Memory::Read16(font+0x168CA))};
    if(begin[0]>=end[0] && begin[1]>=end[1]) return;
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};
    if(NativeGraphicsFrameShouldRender()) {
        call(0x8005AF80);ScreenOrtho();Identity();RasterQuad(true,true);uiStages=1;uiTextured=true;
        auto binding=ReadNativeTextureObject(0x8065B408);
        binding.sampler.min_filter=binding.sampler.mag_filter=1;
        binding.source.min_lod=binding.source.max_lod=0;
        BindNativeTexture(binding);
        const auto records=Memory::Read32(font+0x44);
        for(unsigned layer=0;layer<2;++layer) {
            if(begin[layer]>=end[layer]) continue;
            if(begin[layer]<0 || end[layer]>1024) throw std::runtime_error("Native font glyph batch exceeds its CPU index array");
            const uint32_t count=uint32_t(end[layer]-begin[layer]);
            const auto indexAddress=font+0x15098+layer*0x800+uint32_t(begin[layer])*2;
            uint32_t largest=0;
            for(uint32_t index=0;index<count;++index) largest=std::max(largest,uint32_t(Memory::Read16(indexAddress+index*2)));
            if(largest>=Memory::Read32(font+0x15058)) throw std::runtime_error("Native font glyph index exceeds its CPU records");
            AuroraNativeGeometry geometry{};
            if(Allocate(MH3_UI_QUADS,count*4,geometry)) {
                Checked(mh3_ui_build_glyphs(&GetNativeDrawState(),Span(records,size_t(largest+1)*84),
                    Span(indexAddress,count*2),count,&geometry));
                aurora_submit_native_geometry(&geometry,true);
            }
        }
    }
    // CPU submission cursors advance even when frame admission/draw capacity skips.
    for(unsigned layer=0;layer<2;++layer) if(begin[layer]<end[layer]) Memory::Write16(font+0x168C8+layer*2,uint16_t(end[layer]));
    Invalidate(call);Memory::Write32(font+0x168CC,queued);
}
REGISTER_NATIVE_FUNCTION_AS(0x8005C174,MH3_FontFlush,"FontFlush");
