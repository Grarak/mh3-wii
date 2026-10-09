// Disc/guest address ownership boundary. Lookup algorithms and byte copies are
// portable C. No GX, content/name matching or renderer fallback is present.
#include "abi_bridge.h"
#include "memory.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr size_t capacity=8192;
std::array<mh3_source_range,capacity> ranges{},work{},transfer{};
mh3_source_tracker tracker{};
bool initialized=false;
struct Pack {
    const mh3_mesh_pack* pack;
    const mh3_model_catalog* catalog;
    uintptr_t handle;
    uint64_t generation;
};
std::vector<Pack> packs;
void Checked(mh3_res_status status) {
    if(status!=MH3_RES_OK) throw std::runtime_error("Native model provenance failed: "+std::to_string(status));
}
void Initialize() {
    if(!initialized) {
        Checked(mh3_source_tracker_init(&tracker,ranges.data(),work.data(),transfer.data(),capacity));
        initialized=true;
    }
}
uint64_t NextGeneration() {
    if(tracker.generation==UINT64_MAX) throw std::runtime_error("Native model provenance generation overflow");
    return tracker.generation+1;
}
void Copy(CpuContext* cpu) {
    const uint32_t destination=cpu->gpr[3],source=cpu->gpr[4],bytes=cpu->gpr[5];
    if(bytes) {
        void* dst=Memory::GetPointer(destination,bytes);
        const void* src=Memory::GetPointer(source,bytes);
        // Resolve provenance transaction first; capacity failure leaves guest
        // bytes untouched. Tracking does not inspect either byte payload.
        RecordNativeResourceCopy(destination,source,bytes);
        mh3_model_copy_bytes(dst,src,bytes);
    }
    cpu->gpr[3]=destination;
}
} // namespace

void RecordNativeDiscWrite(uint32_t destination,uint64_t offset,uint32_t bytes) {
    if(!bytes) return;
    Initialize();Checked(mh3_source_record_disc(&tracker,destination,bytes,offset,NextGeneration()));
}
void RecordNativeResourceCopy(uint32_t destination,uint32_t source,uint32_t bytes) {
    if(!bytes || destination==source) return;
    Initialize();
    // Most runtime copies have no disc ownership. Avoid walking all intervals
    // unless a source or overwritten destination intersects tracked bytes.
    if(!mh3_source_intersects(&tracker,source,bytes) &&
       !mh3_source_intersects(&tracker,destination,bytes)) return;
    Checked(mh3_source_record_copy(&tracker,destination,source,bytes,NextGeneration()));
}
void RetireNativeResourceOwner(uint32_t address,uint32_t bytes) {
    if(!bytes) return;
    Initialize();
    if(mh3_source_intersects(&tracker,address,bytes)) Checked(mh3_source_retire(&tracker,address,bytes));
}
void RegisterNativeModelPack(const mh3_mesh_pack* pack,const mh3_model_catalog* catalog,
                             uintptr_t handle,uint64_t generation) {
    if(!handle || !generation || mh3_model_catalog_validate_pack(catalog,pack)!=MH3_PACK_OK)
        throw std::runtime_error("Invalid external native model pack registration");
    for(const auto& p:packs) {
        if(p.handle==handle) throw std::runtime_error("Native model resident handle already registered");
        for(uint32_t i=0;i<catalog->model_count;i++) {
            mh3_catalog_model model{};mh3_model_catalog_model(catalog,i,&model);
            uint32_t found=0;
            if(mh3_model_catalog_find(p.catalog,model.partition_offset,model.model_offset,&found)==MH3_PACK_OK)
                throw std::runtime_error("Ambiguous external native model pack identity");
        }
    }
    packs.push_back({pack,catalog,handle,generation});
}
void UnregisterNativeModelPack(uintptr_t handle) {
    for(auto it=packs.begin();it!=packs.end();++it) if(it->handle==handle) {packs.erase(it);return;}
    throw std::runtime_error("Unknown native model resident handle");
}
NativeModelOwner ResolveNativeModelOwner(uint32_t model_address) {
    Initialize();
    for(const auto& p:packs) {
        uint32_t index=0,container=0;uint64_t generation=0;
        const auto status=mh3_source_resolve_model(&tracker,p.catalog,model_address,&index,&container,&generation);
        if(status==MH3_RES_ABSENT) continue;
        Checked(status);
        mh3_catalog_model model{};mh3_model_catalog_model(p.catalog,index,&model);
        // Provenance selected the owner; these fields validate that the loader
        // still exposes the expected resource, rather than identifying bytes.
        if(Memory::Read32(model_address)!=0x4d444c30 || Memory::Read32(model_address+4)!=model.model_bytes ||
           Memory::Read32(model_address+8)!=model.revision ||
           uint32_t(model_address+Memory::Read32(model_address+12))!=container)
            throw std::runtime_error("Loaded native MDL0 header differs from registered source identity");
        return {p.pack,p.catalog,index,container,model_address,p.handle,p.generation,generation};
    }
    throw std::runtime_error("Model has no complete native loader provenance or external converted pack");
}
mh3_catalog_resource ResolveNativeModelResource(const NativeModelOwner& owner,
                                                uint32_t resource,bool material) {
    if(resource<owner.container_address) throw std::runtime_error("Native model resource precedes its container");
    const uint32_t id=Memory::Read32(resource+(material?12:60));
    mh3_catalog_resource r{};
    if(mh3_model_catalog_find_resource(owner.catalog,owner.catalog_model_index,material,id,&r)!=MH3_PACK_OK ||
       resource-owner.container_address!=r.source_offset || Memory::Read32(resource)!=r.source_bytes ||
       uint32_t(resource+Memory::Read32(resource+4))!=owner.model_address)
        throw std::runtime_error("Native model subresource identity differs from registered source");
    return r;
}
extern "C" void MH3_ModelMemcpy(CpuContext* cpu) {Copy(cpu);}
REGISTER_NATIVE_FUNCTION_AS(0x80004000,MH3_ModelMemcpy,"NativeResourceMemcpy");
extern "C" void MH3_ModelMemmove(CpuContext* cpu) {Copy(cpu);}
REGISTER_NATIVE_FUNCTION_AS(0x8045BC18,MH3_ModelMemmove,"NativeResourceMemmove");
