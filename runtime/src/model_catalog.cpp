// Disc/guest address ownership boundary. Lookup algorithms and byte copies are
// portable C. No GX, content/name matching or renderer fallback is present.
#include "abi_bridge.h"
#include "memory.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include <array>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

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
    std::vector<mh3_prepared_mesh> meshes;
    std::vector<NativePreparedMaterial> materials;
    std::vector<uint32_t> material_catalog_indices;
    std::vector<std::array<mh3_catalog_material_block,5>> material_blocks;
};
std::vector<Pack> packs;
uint64_t source_epoch=0;
struct CachedOwner {uint64_t epoch;NativeModelOwner owner;};
struct CachedResource {uint64_t epoch,generation;mh3_catalog_resource resource;};
struct CachedBlock {uint64_t epoch,source_generation;uintptr_t resident_handle;uint64_t resident_generation;uint32_t material_index;};
std::unordered_map<uint32_t,CachedOwner> owners;
std::unordered_map<uint64_t,CachedResource> resources;
std::unordered_map<uint64_t,CachedBlock> blocks;
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
uint32_t Canonical(uint32_t address,uint32_t bytes) {
    uint32_t canonical=0;Checked(mh3_source_canonical_address(address,bytes,&canonical));return canonical;
}
void InvalidateSourceCache() {
    if(source_epoch==UINT64_MAX) throw std::runtime_error("Native model source cache epoch overflow");
    ++source_epoch;
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
    Initialize();destination=Canonical(destination,bytes);
    Checked(mh3_source_record_disc(&tracker,destination,bytes,offset,NextGeneration()));InvalidateSourceCache();
}
void RecordNativeResourceCopy(uint32_t destination,uint32_t source,uint32_t bytes) {
    if(!bytes || destination==source) return;
    Initialize();destination=Canonical(destination,bytes);source=Canonical(source,bytes);
    if(destination==source) return;
    // Most runtime copies have no disc ownership. Avoid walking all intervals
    // unless a source or overwritten destination intersects tracked bytes.
    if(!mh3_source_intersects(&tracker,source,bytes) &&
       !mh3_source_intersects(&tracker,destination,bytes)) return;
    Checked(mh3_source_record_copy(&tracker,destination,source,bytes,NextGeneration()));InvalidateSourceCache();
}
void RetireNativeResourceOwner(uint32_t address,uint32_t bytes) {
    if(!bytes) return;
    Initialize();address=Canonical(address,bytes);
    if(mh3_source_intersects(&tracker,address,bytes)) {
        Checked(mh3_source_retire(&tracker,address,bytes));InvalidateSourceCache();
    }
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
    Pack admitted{pack,catalog,handle,generation,{},{},{},{}};
    admitted.meshes.resize(pack->counts[MH3_PACK_MESHES]);
    for(uint32_t i=0;i<catalog->model_count;i++) {
        mh3_catalog_model model{};mh3_model_catalog_model(catalog,i,&model);
        for(uint32_t j=0;j<model.shape_count;j++) {
            mh3_catalog_resource resource{};mh3_model_catalog_resource(catalog,0,model.shape_first+j,&resource);
            if(resource.pack_index==UINT32_MAX) continue;
            if(mh3_model_mesh_prepare(catalog,pack,i,resource.source_id,handle,generation,
                                      &admitted.meshes[resource.pack_index])!=MH3_PACK_OK)
                throw std::runtime_error("Native model immutable admission failed");
        }
    }
    admitted.materials.resize(pack->counts[MH3_PACK_MATERIALS]);
    admitted.material_catalog_indices.resize(admitted.materials.size(),UINT32_MAX);
    admitted.material_blocks.resize(admitted.materials.size());
    for(uint32_t i=0;i<catalog->material_count;i++) {
        mh3_catalog_resource resource{};mh3_model_catalog_resource(catalog,1,i,&resource);
        admitted.material_catalog_indices.at(resource.pack_index)=i;
        if(catalog->version==2) for(uint32_t kind=0;kind<5;kind++) {
            if(mh3_model_catalog_block(catalog,i*5+kind,&admitted.material_blocks.at(resource.pack_index)[kind])!=MH3_PACK_OK)
                throw std::runtime_error("Native material block admission failed");
        }
    }
    for(uint32_t i=0;i<admitted.materials.size();i++) {
        if(mh3_model_material_prepare(pack,i,&admitted.materials[i])!=MH3_PACK_OK)
            throw std::runtime_error("Native model requires complete supported typed material payload");
        const auto& material=admitted.materials[i];
        const uint32_t required=(1u<<material.state.texgen_count)-1;
        if((material.texcoord_map_known&required)!=required)
            throw std::runtime_error("Native model requires M3MT v3 exact texture-coordinate map provenance");
    }
    packs.push_back(std::move(admitted));owners.clear();resources.clear();blocks.clear();
}
void UnregisterNativeModelPack(uintptr_t handle) {
    for(auto it=packs.begin();it!=packs.end();++it) if(it->handle==handle) {
        packs.erase(it);owners.clear();resources.clear();blocks.clear();return;
    }
    throw std::runtime_error("Unknown native model resident handle");
}
NativeModelOwner ResolveNativeModelOwner(uint32_t model_address) {
    Initialize();
    model_address=Canonical(model_address,16);
    const auto cached=owners.find(model_address);
    if(cached!=owners.end() && cached->second.epoch==source_epoch) return cached->second.owner;
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
           Canonical(model_address+Memory::Read32(model_address+12),1)!=container)
            throw std::runtime_error("Loaded native MDL0 header differs from registered source identity");
        NativeModelOwner owner{p.pack,p.catalog,index,container,model_address,p.handle,p.generation,generation};
        owners.insert_or_assign(model_address,CachedOwner{source_epoch,owner});return owner;
    }
    throw std::runtime_error("Model has no complete native loader provenance or external converted pack");
}
mh3_catalog_resource ResolveNativeModelResource(const NativeModelOwner& owner,
                                                uint32_t resource,bool material) {
    const NativeModelOwner current=ResolveNativeModelOwner(owner.model_address);
    if(current.pack!=owner.pack || current.catalog!=owner.catalog ||
       current.resident_handle!=owner.resident_handle || current.resident_generation!=owner.resident_generation ||
       current.source_generation!=owner.source_generation || current.catalog_model_index!=owner.catalog_model_index)
        throw std::runtime_error("Native model owner was invalidated");
    resource=Canonical(resource,material?16:64);
    const uint64_t key=(uint64_t(resource)<<1)|uint32_t(material);
    const auto cached=resources.find(key);
    if(cached!=resources.end() && cached->second.epoch==source_epoch && cached->second.generation==owner.source_generation) {
        if(cached->second.resource.model_index!=owner.catalog_model_index)
            throw std::runtime_error("Native model resource cache has a different owner");
        return cached->second.resource;
    }
    if(resource<owner.container_address) throw std::runtime_error("Native model resource precedes its container");
    const uint32_t id=Memory::Read32(resource+(material?12:60));
    mh3_catalog_resource r{};
    if(mh3_model_catalog_find_resource(owner.catalog,owner.catalog_model_index,material,id,&r)!=MH3_PACK_OK ||
       resource-owner.container_address!=r.source_offset || Memory::Read32(resource)!=r.source_bytes ||
       Canonical(resource+Memory::Read32(resource+4),1)!=owner.model_address)
        throw std::runtime_error("Native model subresource identity differs from registered source");
    resources.insert_or_assign(key,CachedResource{source_epoch,owner.source_generation,r});return r;
}
const mh3_prepared_mesh& ReadNativePreparedMesh(const NativeModelOwner& owner,uint32_t shape) {
    const mh3_catalog_resource resource=ResolveNativeModelResource(owner,shape,false);
    if(resource.pack_index==UINT32_MAX) throw std::runtime_error("Native model shape has no converted mesh");
    for(const auto& pack:packs) if(pack.handle==owner.resident_handle && pack.generation==owner.resident_generation)
        return pack.meshes.at(resource.pack_index);
    throw std::runtime_error("Native model resident allocation was retired");
}
const NativePreparedMaterial& ReadNativePreparedMaterial(const NativeModelOwner& owner,uint32_t material) {
    const mh3_catalog_resource resource=ResolveNativeModelResource(owner,material,true);
    for(const auto& pack:packs) if(pack.handle==owner.resident_handle && pack.generation==owner.resident_generation)
        return pack.materials.at(resource.pack_index);
    throw std::runtime_error("Native model material resident allocation was retired");
}
NativeMaterialBlockDefaults ReadNativeMaterialBlockDefaults(const NativeModelOwner& owner,
    uint32_t material,mh3_model_material_block kind,uint32_t actual_block) {
    const mh3_catalog_resource resource=ResolveNativeModelResource(owner,material,true);
    if(kind<0 || kind>=MH3_MODEL_MATERIAL_BLOCK_COUNT)
        throw std::runtime_error("Invalid native material block kind");
    for(const auto& pack:packs) if(pack.handle==owner.resident_handle && pack.generation==owner.resident_generation) {
        const auto& block=pack.material_blocks.at(resource.pack_index)[kind];
        if(!(block.flags&1)) throw std::runtime_error("Native material block requires verified M3ID v2 ownership");
        actual_block=Canonical(actual_block,block.source_bytes);
        // Material tables have 128-byte records in a U32-sized metadata span,
        // so their admitted index fits the lower29 bits. Keep shared TEV
        // scopes separate instead of evicting each other on alternate draws.
        const uint64_t key=(uint64_t(actual_block)<<32)|(uint64_t(resource.pack_index)<<3)|uint32_t(kind);
        const auto cached=blocks.find(key);
        uint64_t generation=0;
        if(cached!=blocks.end() && cached->second.epoch==source_epoch &&
           cached->second.resident_handle==owner.resident_handle && cached->second.resident_generation==owner.resident_generation &&
           cached->second.material_index==resource.pack_index) generation=cached->second.source_generation;
        else {
            Checked(mh3_source_resolve_material_block(&tracker,owner.catalog,actual_block,
                pack.material_catalog_indices.at(resource.pack_index),uint32_t(kind),&generation));
            blocks.insert_or_assign(key,CachedBlock{source_epoch,generation,owner.resident_handle,owner.resident_generation,resource.pack_index});
        }
        return {&pack.materials.at(resource.pack_index),actual_block,block.source_bytes,kind,generation};
    }
    throw std::runtime_error("Native material block resident owner was retired");
}
extern "C" void MH3_ModelMemcpy(CpuContext* cpu) {Copy(cpu);}
REGISTER_NATIVE_FUNCTION_AS(0x80004000,MH3_ModelMemcpy,"NativeResourceMemcpy");
extern "C" void MH3_ModelMemmove(CpuContext* cpu) {Copy(cpu);}
REGISTER_NATIVE_FUNCTION_AS(0x8045BC18,MH3_ModelMemmove,"NativeResourceMemmove");
