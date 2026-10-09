// Thin guest ABI adapters for typed scene feedback plans. GPU targets retain
// snapshots; guest texture addresses are identities, never readback buffers.
#include "abi_bridge.h"
#include "guest_call.h"
#include "native_graphics.h"
#include "memory.h"
#include "../native/mh3_scene_feedback.h"
#include "../native/adapters/model_state_bridge.h"
#include <aurora/native_scene_feedback.hpp>
#include <cstring>
#include <cmath>
#include <stdexcept>

namespace {
using namespace MemoryInline;
const void* Identity(uint32_t address){if(!address)return nullptr;return Memory::GetPointer(address,1);}
void Require(int success,const char *reason){if(!success)throw std::runtime_error(reason);}
bool Capture(const mh3_scene_capture_plan& plan){
    aurora::SceneCaptureParameters parameters;parameters.width=plan.width;parameters.height=plan.height;parameters.format=plan.format;parameters.box_filter=plan.box_filter;
    for(uint32_t i=0;i<4;i++)parameters.source[i]=plan.source[i];return aurora::publish_scene_capture(Identity(plan.identity),parameters);
}
void State(const mh3_scene_quad_plan& plan,bool bind=true){
    SetNativeProjection(plan.projection,false);SetNativePositionMatrix(plan.position);SetNativeMaterial(plan.material);SetNativeRaster(plan.raster);
    mh3_native_texture_binding binding{};binding.source.resource_address=binding.source.payload_address=plan.identity;binding.source.width=plan.width;binding.source.height=plan.height;binding.source.format=plan.format;binding.source.levels=1;binding.source.mutable_source=1;binding.source.generation=aurora::scene_feedback_texture_version(Identity(plan.identity),plan.width,plan.height);binding.map_id=0;binding.sampler.min_filter=binding.sampler.mag_filter=1;
    if(!bind||!NativeGraphicsFrameShouldRender())return;
    if(!binding.source.generation)throw std::runtime_error("Scene feedback sampled before GPU publication");BindNativeTexture(binding);
}
void Emit(const mh3_scene_quad_plan& plan){
    State(plan);
    if(!NativeGraphicsFrameShouldRender())return;
    AuroraNativeMaterialGeometry geometry{};if(!aurora_allocate_native_material_geometry(4,6,&geometry))return;
    for(uint32_t i=0;i<4;i++){
        auto &v=geometry.vertices[i];std::memset(&v,0,sizeof(v));AuroraNativeVertex prepared{};mh3_draw_vertex(&GetNativeDrawState(),plan.vertices[i],plan.uv[i],plan.color,&prepared);std::memcpy(v.position,prepared.position,16);std::memcpy(v.view_position,plan.vertices[i],12);v.normal[0]=1;std::memcpy(v.color[0],plan.color,4);std::memcpy(v.color[1],plan.color,4);
        for(uint32_t map=0;map<8;map++){v.texcoord[map][0]=plan.uv[i][0];v.texcoord[map][1]=plan.uv[i][1];v.texcoord[map][2]=1;}
    }
    const uint16_t indices[6]={0,1,2,0,2,3};std::memcpy(geometry.indices,indices,12);SubmitNativeMaterialGeometry(geometry);
}
mh3_temporal_feedback_input TemporalInput(uint32_t self){
    mh3_temporal_feedback_input input{};input.identity=FlatRead32(self+0x10);input.frames=FlatRead32(self+0x34);input.screen_width=FlatRead16(0x8065b4bc);input.screen_height=FlatRead16(0x8065b4be);input.projection_height=FlatReadFloat32(0x8065b4c4);input.offset[0]=FlatReadFloat32(self+8);input.offset[1]=FlatReadFloat32(self+12);
    input.enabled=FlatRead8(self);input.mode=FlatRead8(self+1);input.alpha=FlatRead8(self+2);input.amount=FlatRead8(self+3);input.threshold_mode=FlatRead8(self+4);input.threshold_alpha=FlatRead8(self+5);input.threshold_gray=FlatRead8(self+6);input.subtract_scale=uint8_t(FlatRead32(0x80796d80));
    for(uint32_t i=0;i<3;i++)input.tint[i]=FlatRead8(self+0x14+i);return input;
}
void SceneDrawBack(CpuContext *cpu,uint32_t self,int subtract){
    uint32_t id=FlatRead32(self+0x134);if(!id)return;GuestCall call{cpu};bool paused=call.Call<0x80046F70>()!=0;mh3_scene_quad_plan plan{};
    Require(mh3_scene_capture_quad(id,FlatReadFloat32(self+0x120),FlatReadFloat32(self+0x124),FlatReadFloat32(self+0x128),FlatReadFloat32(self+0x12c),paused?120:255,subtract,&GetNativeDrawState(),&GetNativeMaterialState(),&plan),"Invalid captured scene quad");Emit(plan);
}
void SceneCapture(uint32_t self,uint32_t conditional){
    uint32_t id=FlatRead32(self+0x134),mode=FlatRead8(self+0x13c);if(!id||(conditional&&mode!=0&&mode!=2))return;
    float w=FlatReadFloat32(self+0x120),h=FlatReadFloat32(self+0x124);if(!std::isfinite(w)||!std::isfinite(h)||w<1||h<1||w>65535||h>65535)throw std::runtime_error("Invalid scene capture dimensions");
    mh3_scene_capture_plan plan{id,uint16_t(int32_t(w)),uint16_t(int32_t(h)),6,{0,0,w,h},0};Capture(plan);
}
void GlareBrightPass(uint32_t self){
    mh3_scene_glare_input input{};input.identity=FlatRead32(self+8);input.levels=int32_t(FlatRead32(self+0x10));input.start_width=int16_t(FlatRead16(self+0x1c));input.start_height=int16_t(FlatRead16(self+0x1e));input.shift=FlatRead32(self+0x18);input.low_threshold=FlatRead32(self+0x28);input.high_threshold=FlatRead32(self+0x2c);input.darken=FlatReadFloat32(self+0x30);input.brighten=FlatReadFloat32(self+0x34);input.projection_width=FlatReadFloat32(0x8066d298);input.projection_height=FlatReadFloat32(0x8066d29c);
    mh3_scene_glare_plan plan;Require(mh3_scene_glare_prepare(&input,&plan),"Invalid glare resource dimensions or threshold");
    /* Authored result dimensions are CPU state even during a skipped draw. */
    Memory::Write16(self+0x14,plan.width);Memory::Write16(self+0x16,plan.height);
    aurora::SceneGlareParameters parameters;parameters.width=plan.width;parameters.height=plan.height;parameters.low_threshold=plan.low_threshold;parameters.high_threshold=plan.high_threshold;parameters.darken=plan.darken;parameters.brighten=plan.brighten;parameters.shift_scale=plan.shift_scale;
    for(uint32_t i=0;i<plan.level_count;i++){const auto &level=plan.levels[i];parameters.levels.push_back({level.source_width,level.source_height,level.width,level.height});}aurora::publish_scene_glare(Identity(plan.identity),parameters);
    if(plan.level_count){mh3_scene_quad_plan state;Require(mh3_scene_capture_quad(input.identity,plan.width,plan.height,input.projection_width,input.projection_height,255,0,&GetNativeDrawState(),&GetNativeMaterialState(),&state),"Invalid glare inherited quad state");State(state);}
}
uint32_t CpuRead(void*,uint32_t address,unsigned bytes){if(bytes==1)return FlatRead8(address);if(bytes==2)return FlatRead16(address);return FlatRead32(address);}
void CpuWrite(void*,uint32_t address,uint32_t value,unsigned bytes){if(bytes==1)FlatWrite8(address,uint8_t(value));else if(bytes==2)Memory::Write16(address,uint16_t(value));else FlatWrite32(address,value);}
uint32_t CpuCall(void* context,uint32_t fn,const uint32_t *args,unsigned count){auto cpu=static_cast<CpuContext*>(context);if(count>8)throw std::runtime_error("Scene CPU call exceeds guest integer ABI");for(unsigned i=0;i<count;i++)cpu->gpr[3+i]=args[i];InvokeIndirectCpu(fn,cpu);return cpu->gpr[3];}
void CpuCopy(void*,uint32_t destination,uint32_t source,uint32_t bytes){std::memcpy(Memory::GetPointer(destination,bytes),Memory::GetPointer(source,bytes),bytes);}
void CpuZero(void*,uint32_t destination,uint32_t bytes){std::memset(Memory::GetPointer(destination,bytes),0,bytes);}
void CpuStringCopy(void*,uint32_t destination,uint32_t source){unsigned length=0;while(length<320&&FlatRead8(source+length))length++;if(length==320)throw std::runtime_error("Equipment resource name exceeds authored scratch buffer");CpuCopy(nullptr,destination,source,length+1);}
int CpuStringCompare(void*,uint32_t first,uint32_t second){for(uint32_t i=0;i<320;i++){const uint8_t a=FlatRead8(first+i),b=FlatRead8(second+i);if(a!=b||!a)return int(a)-int(b);}throw std::runtime_error("Equipment material name lacks terminator");}
void CpuTextureObject(void*,uint32_t object,uint32_t payload){mh3_native_texture_binding binding{};binding.source.resource_address=binding.source.payload_address=payload;binding.source.width=binding.source.height=128;binding.source.format=6;binding.source.levels=1;binding.source.mutable_source=1;binding.source.generation=ReadNativeTextureGeneration(payload);binding.sampler.min_filter=binding.sampler.mag_filter=1;binding.sampler.edge_lod=1;StoreNativeTextureObject(object,binding);}
mh3_scene_cpu_ops CpuOps(CpuContext *cpu){return {cpu,CpuRead,CpuWrite,CpuCall,CpuCopy,CpuZero,CpuStringCopy,CpuStringCompare,CpuTextureObject};}
}
extern "C" void MH3_NativeTemporalFeedback(CpuContext *cpu){
    uint32_t self=cpu->gpr[3];auto input=TemporalInput(self);mh3_temporal_feedback_plan plan;Require(mh3_scene_temporal_prepare(&input,&GetNativeDrawState(),&GetNativeMaterialState(),&plan),"Invalid temporal feedback resource");if(!plan.enabled)return;FlatWrite32(self+0x34,plan.frames);
    State(plan.initial,false);if(plan.frames!=1)State(plan.previous);if(plan.draw_previous)Emit(plan.previous);Capture(plan.capture);State(plan.frames==1?plan.initial:plan.previous);if(plan.draw_threshold)Emit(plan.threshold);
}
extern "C" void MH3_NativeFeedbackThreshold(CpuContext *cpu){auto input=TemporalInput(cpu->gpr[3]);input.enabled=1;mh3_temporal_feedback_plan plan;Require(mh3_scene_temporal_prepare(&input,&GetNativeDrawState(),&GetNativeMaterialState(),&plan),"Invalid feedback threshold resource");if(plan.draw_threshold)Emit(plan.threshold);else {
    /* The original binds the history image before testing the mode. */
    mh3_native_texture_binding binding{};binding.source.resource_address=binding.source.payload_address=input.identity;binding.source.width=input.screen_width/2;binding.source.height=input.screen_height/2;binding.source.format=4;binding.source.levels=1;binding.source.mutable_source=1;binding.source.generation=aurora::scene_feedback_texture_version(Identity(input.identity),binding.source.width,binding.source.height);binding.sampler.min_filter=binding.sampler.mag_filter=1;
    if(NativeGraphicsFrameShouldRender()){Require(binding.source.generation!=0,"Feedback threshold sampled unpublished history");BindNativeTexture(binding);}
}}
extern "C" void MH3_NativeSceneCapture(CpuContext *cpu){SceneCapture(cpu->gpr[3],cpu->gpr[4]);}
extern "C" void MH3_NativeSceneDrawBack(CpuContext *cpu){GuestFrame frame(cpu,0x10);SceneDrawBack(cpu,frame.Arg(3),uint8_t(frame.Arg(4))==1);}
extern "C" void MH3_NativeSavedSceneDraw(CpuContext *cpu){
    uint32_t id=FlatRead32(0x8066d2b0);if(!id)return;mh3_scene_quad_plan plan;Require(mh3_scene_capture_quad(id,FlatReadFloat32(0x8066d298),FlatReadFloat32(0x8066d29c),FlatReadFloat32(0x8066d2a0),FlatReadFloat32(0x8066d2a4),uint8_t(cpu->gpr[3])?200:255,0,&GetNativeDrawState(),&GetNativeMaterialState(),&plan),"Invalid saved scene quad");Emit(plan);
}
extern "C" void MH3_NativeGlareBrightPass(CpuContext *cpu){GlareBrightPass(cpu->gpr[3]);}
extern "C" void MH3_NativeGlareDraw(CpuContext *cpu){
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};uint32_t self=frame.Arg(3);if(call.Call<0x80046F70>()==0&&FlatRead8(0x8066d294))SceneCapture(0x8066d178,0);if(!FlatRead8(self))return;
    GlareBrightPass(self);SceneDrawBack(cpu,0x8066d178,0);mh3_scene_quad_plan plan;Require(mh3_scene_capture_quad(FlatRead32(self+8),FlatRead16(self+0x14),FlatRead16(self+0x16),FlatReadFloat32(0x8066d2a0),FlatReadFloat32(0x8066d2a4),255,0,&GetNativeDrawState(),&GetNativeMaterialState(),&plan),"Invalid glare composite quad");
    for(uint32_t i=0;i<3;i++)plan.color[i]=FlatRead8(self+2+i);plan.raster.src_factor=FlatRead8(self+5);plan.raster.dst_factor=FlatRead8(self+6);Emit(plan);
}
extern "C" void MH3_EffectTexCoordGenerator(CpuContext *cpu);
extern "C" void MH3_NativeSceneTexgen(CpuContext *cpu){cpu->gpr[7]=0;cpu->gpr[8]=125;MH3_EffectTexCoordGenerator(cpu);}
extern "C" void MH3_NativeSavedSceneCapture(CpuContext*){
    const uint32_t id=FlatRead32(0x8066d2b0);if(!id)return;const float w=FlatReadFloat32(0x8066d298),h=FlatReadFloat32(0x8066d29c);if(!std::isfinite(w)||!std::isfinite(h)||w<1||h<1||w>65535||h>65535)throw std::runtime_error("Invalid saved capture dimensions");
    Capture({id,uint16_t(int32_t(w)),uint16_t(int32_t(h)),6,{0,0,w,h},0});
}
extern "C" void MH3_NativeGrabScreen(CpuContext *cpu){
    GuestFrame frame(cpu,0x20);GuestCall call{cpu};if(FlatRead8(0x8065aa90)||FlatRead8(0x8065aa65))return;
    call.Call<0x80040354>({0x8066d2a0});call.Call<0x802AEA18>();call.Call<0x800591A0>({0x8066d178});call.Call<0x8005921C>({0x8066d178});
    if(FlatRead8(0x8065b2c8)!=1)return;const uint32_t player=call.Call<0x800CFAF4>({0});if(player&&FlatRead8(player+0x112))return;if(call.Call<0x800582D4>({0})==2)return;
    if(NativeGraphicsFrameShouldRender())Require(aurora::clone_scene_feedback_texture(Identity(FlatRead32(0x8066d2b0)),Identity(FlatRead32(0x8066d2ac))),"GrabScreen source was not captured");FlatWrite8(0x8065b2c8,2);
}
extern "C" void MH3_NativeSceneEffects(CpuContext *cpu){
    GuestFrame frame(cpu,0x20);GuestCall call{cpu};if(uint8_t(call.Call<0x800CF26C>())==2&&FlatRead8(0x806ad0f0)==1)return;
    call.Call<0x8005AF80>();if(call.Call<0x80046F70>()==1){SceneDrawBack(cpu,0x8066d178,0);for(uint32_t slot=1;slot<6;slot++)if(FlatRead8(0x8066d17c+slot)==2&&(FlatRead8(0x8066d17a)&4)){const uint32_t fn=FlatRead32(0x8066d18c);if(fn)call(fn,{FlatRead32(0x8066d1a4)});}}
    else if(!FlatRead8(0x8065b2c9)||FlatRead8(0x8065b2c8)!=2){call.Call<0x80059060>({0x8066d178});for(uint32_t slot=1;slot<6;slot++){const uint32_t stage=FlatRead8(0x8066d17c+slot);if(stage&&(uint32_t(FlatRead8(0x8066d17a))&(1u<<(stage&31)))){const uint32_t fn=FlatRead32(0x8066d184+stage*4);if(fn)call(fn,{FlatRead32(0x8066d19c+stage*4)});}}}
    call.Call<0x800885F4>({0x7ff});auto material=GetNativeMaterialState();material.indirect_count=0;SetNativeMaterial(material);
}
extern "C" void MH3_NativeEquipmentTextures(CpuContext *cpu){GuestFrame frame(cpu,0x160);const auto ops=CpuOps(cpu);Require(mh3_scene_equipment_update(&ops,frame.Arg(3),frame.Arg(4),frame.Arg(5),frame[0]),"Invalid equipment texture update");}
REGISTER_NATIVE_FUNCTION_AS(0x80055FBC,MH3_NativeTemporalFeedback,"NativeTemporalFeedback");
REGISTER_NATIVE_FUNCTION_AS(0x8005666C,MH3_NativeFeedbackThreshold,"NativeFeedbackThreshold");
REGISTER_NATIVE_FUNCTION_AS(0x80056660,MH3_NativeSceneTexgen,"NativeFeedbackTexgen");
REGISTER_NATIVE_FUNCTION_AS(0x80057348,MH3_NativeSceneTexgen,"NativeGlareTexgen");
REGISTER_NATIVE_FUNCTION_AS(0x80057354,MH3_NativeGlareBrightPass,"NativeGlareBrightPass");
REGISTER_NATIVE_FUNCTION_AS(0x8005876C,MH3_NativeSceneCapture,"NativeSceneCapture");
REGISTER_NATIVE_FUNCTION_AS(0x80058870,MH3_NativeSceneDrawBack,"NativeSceneDrawBack");
REGISTER_NATIVE_FUNCTION_AS(0x80058C08,MH3_NativeSceneTexgen,"NativeSceneTexgen");
REGISTER_NATIVE_FUNCTION_AS(0x80058C34,MH3_NativeSavedSceneDraw,"NativeSavedSceneDraw");
REGISTER_NATIVE_FUNCTION_AS(0x80058F84,MH3_NativeSavedSceneCapture,"NativeSavedSceneCapture");
REGISTER_NATIVE_FUNCTION_AS(0x800586A0,MH3_NativeGrabScreen,"NativeGrabScreen");
REGISTER_NATIVE_FUNCTION_AS(0x80059270,MH3_NativeSceneEffects,"NativeSceneEffects");
REGISTER_NATIVE_FUNCTION_AS(0x80268914,MH3_NativeEquipmentTextures,"NativeEquipmentTextures");
