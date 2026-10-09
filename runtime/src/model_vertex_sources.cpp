// Pure array binding ABI: retain typed source provenance instead of GXSetArray.
#include "abi_bridge.h"
#include "memory.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include <array>
#include <stdexcept>
#include <unordered_map>

namespace {
std::array<mh3_native_vertex_source,12> bindings{};
std::array<bool,12> known{};
std::unordered_map<uint32_t,uint64_t> generations;
uint64_t binding_generation=0;
void Bind(CpuContext* cpu,mh3_vertex_kind kind,uint32_t semantic) {
    if(!Memory::Read32(cpu->gpr[3])) return; // Original invalid optional resource.
    StoreNativeVertexSource(semantic,ReadNativeVertexResource(cpu->gpr[3],kind));
}
}
void MarkNativeVertexProduced(uint32_t payload,uint64_t generation) {
    if(!payload || !generation || (generations.contains(payload) && generation<generations.at(payload)))
        throw std::runtime_error("Invalid native vertex producer generation");
    generations.insert_or_assign(payload,generation);
    for(uint32_t i=0;i<12;i++) if(known[i] && bindings[i].payload_address==payload)
        bindings[i].source_generation=generation;
}
mh3_native_vertex_source ReadNativeVertexResource(uint32_t reference,mh3_vertex_kind kind) {
    const uint32_t resource=Memory::Read32(reference),size=Memory::Read32(resource);
    mh3_native_vertex_source source{};
    if(mh3_vertex_source_read({Memory::GetPointer(resource,size),size},resource,kind,&source)!=MH3_RES_OK)
        throw std::runtime_error("Invalid native vertex array resource");
    const auto generation=generations.find(source.payload_address);
    if(generation!=generations.end()) source.source_generation=generation->second;
    return source;
}
void StoreNativeVertexSource(uint32_t semantic,const mh3_native_vertex_source& source) {
    if(semantic>=12 || !source.payload_address || binding_generation==UINT64_MAX)
        throw std::runtime_error("Invalid native vertex source binding");
    bindings[semantic]=source;bindings[semantic].binding_generation=++binding_generation;known[semantic]=true;
}
bool ReadNativeVertexSource(uint32_t semantic,mh3_native_vertex_source* out) {
    if(semantic>=12 || !out || !known[semantic]) return false;
    *out=bindings[semantic];return true;
}
extern "C" void MH3_ModelPositionArray(CpuContext* cpu) {Bind(cpu,MH3_VERTEX_POSITION,0);}
REGISTER_NATIVE_FUNCTION_AS(0x80088E88,MH3_ModelPositionArray,"NativeModelPositionArray");
extern "C" void MH3_ModelNormalArray(CpuContext* cpu) {Bind(cpu,MH3_VERTEX_NORMAL,1);}
REGISTER_NATIVE_FUNCTION_AS(0x80089000,MH3_ModelNormalArray,"NativeModelNormalArray");
extern "C" void MH3_ModelColorArray(CpuContext* cpu) {
    const uint32_t slot=cpu->gpr[4];
    if(slot==11 || slot==12) Bind(cpu,MH3_VERTEX_COLOR,slot-9);
}
REGISTER_NATIVE_FUNCTION_AS(0x80089178,MH3_ModelColorArray,"NativeModelColorArray");
extern "C" void MH3_ModelFurPositionArray(CpuContext* cpu) {
    const uint32_t resource=Memory::Read32(cpu->gpr[3]);if(!resource) return;
    const uint32_t size=Memory::Read32(resource);mh3_native_vertex_source source{};
    if(mh3_vertex_fur_source_read({Memory::GetPointer(resource,size),size},resource,
                                 uint16_t(cpu->gpr[4]),&source)!=MH3_RES_OK)
        throw std::runtime_error("Invalid native fur position sample");
    const auto generation=generations.find(source.payload_address);
    if(generation!=generations.end()) source.source_generation=generation->second;
    StoreNativeVertexSource(0,source);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008961C,MH3_ModelFurPositionArray,"NativeModelFurPositionArray");
