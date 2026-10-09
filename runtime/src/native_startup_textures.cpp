// Compatibility addresses stay at this ABI boundary. The ordinary C core
// owns readiness, group traversal, sampler policy and selection publication.
#include "abi_bridge.h"
#include "guest_call.h"
#include "native_graphics.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include "runtime/native/mh3_startup_textures.h"
#include "runtime/native/mh3_ui.h"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace {
constexpr uint32_t State=0x8066B968,Selectors=0x8066BDB0,Selection=0x8066BDBA;
constexpr uint32_t Objects[]={0x8066B990,0x8066BA10,0x8066BA90};
struct Startup { GuestCall call;uint32_t archives[3];uint64_t generation; };
mh3_res_status Archive(void *user,uint32_t group,uint32_t *count) {
    auto& startup=*static_cast<Startup*>(user);
    startup.call(0x804E4C30,{startup.archives[group]}); // pure CPU pointer relocation
    *count=Memory::Read32(startup.archives[group]+4);
    return MH3_RES_OK;
}
mh3_res_status Image(void *user,uint32_t group,uint32_t index,mh3_native_texture_binding *out) {
    auto& startup=*static_cast<Startup*>(user);
    const auto descriptor=startup.call(0x804E4D50,{startup.archives[group],index});
    const auto header=Memory::Read32(descriptor);
    mh3_ui_texture_header image{};
    const auto status=mh3_ui_texture_header_read({Memory::GetPointer(header,36),36},&image);
    if(status!=MH3_RES_OK) return status;
    startup.generation=std::max(startup.generation,ReadNativeTextureGeneration(image.payload_address));
    if(startup.generation==UINT64_MAX) return MH3_RES_RANGE;
    ++startup.generation;
    MarkNativeTextureProduced(image.payload_address,startup.generation);
    out->source.resource_address=header;
    out->source.payload_address=image.payload_address;
    out->source.width=image.width;
    out->source.height=image.height;
    out->source.format=image.format;
    out->source.generation=startup.generation;
    out->sampler.wrap_s=image.wrap_s;
    out->sampler.wrap_t=image.wrap_t;
    return MH3_RES_OK;
}
mh3_res_status Publish(void*,uint32_t group,uint32_t slot,const mh3_native_texture_binding *binding) {
    const uint64_t object=uint64_t(Objects[group])+uint64_t(slot)*32;
    if(object>UINT32_MAX || !Memory::Contains(uint32_t(object),32)) return MH3_RES_RANGE;
    StoreNativeTextureObject(uint32_t(object),*binding);
    return MH3_RES_OK;
}
}

extern "C" void MH3_StartupTextures(CpuContext *cpu) {
    GuestFrame frame(cpu,0x10);
    mh3_startup_texture_state state{};
    state.initialized=Memory::Read8(State);
    for(unsigned group=0;group<3;++group) state.loaded[group]=Memory::Read8(State+16+group);
    if(state.initialized || state.loaded[0]!=2 || state.loaded[1]!=2 || state.loaded[2]!=2) return;
    Startup startup{{cpu},{Memory::Read32(State+4),Memory::Read32(State+8),Memory::Read32(State+12)},0};
    const mh3_startup_texture_callbacks callbacks{&startup,Archive,Image,Publish};
    const auto status=mh3_startup_textures_prepare(&state,frame.Arg(3),
        {Memory::GetPointer(Selectors,10),10},&callbacks);
    if(status!=MH3_RES_OK) throw std::runtime_error("Native startup TPL preparation failed: "+std::to_string(status));
    for(unsigned byte=0;byte<4;++byte) Memory::Write8(Selection+byte,state.selection[byte]);
    Memory::Write8(State,state.initialized);
    startup.call(0x800885F4,{0x7FF});
    auto material=GetNativeMaterialState();
    material.indirect_count=0;
    SetNativeMaterial(material);
}
REGISTER_NATIVE_FUNCTION_AS(0x80047954,MH3_StartupTextures,"StartupTextures");
