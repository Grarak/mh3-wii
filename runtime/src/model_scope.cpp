// Native model material/rigid submission boundary. Portable C owns decoding,
// order, coordinate scales and matrix math; these adapters own CPU records and
// forward direct material/mesh publications. No guest/GX/FIFO calls.
#include "abi_bridge.h"
#include "memory.h"
#include "native_graphics.h"
#include "runtime/native/adapters/model_scope_bridge.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/adapters/model_material_bridge.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include "runtime/native/mh3_model_material_prep.h"
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

const mh3_res_memory& MH3NativeResourceMemory();
namespace {
constexpr uint32_t genmode=0x80685300,coordinate_cache=0x8068530c,sync_address=0x80796db0;
constexpr mh3_matrix_runtime_locations locations{0x80685530,0x80685510,0x80796db4,0x80688d94,0x80686368};
mh3_model_texcoord_sizes map_sizes{},coordinate_sizes{};
uint8_t coordinate_map_known=0;
uint32_t pipeline_address=0,operator_address=0;
uint64_t pipeline_revision=0,direct_revision=0;
mh3_indirect_output current_operator{};
void Checked(mh3_res_status status) {
    if(status!=MH3_RES_OK) throw std::runtime_error("Native model scope failed: "+std::to_string(status));
}
void PackChecked(mh3_pack_status status) {
    if(status!=MH3_PACK_OK) throw std::runtime_error("Native model resident mesh failed: "+std::to_string(status));
}
mh3_res_span Bytes(uint32_t address,size_t size) {return {Memory::GetPointer(address,size),size};}
mh3_res_mut_span Mutable(uint32_t address,size_t size) {return {Memory::GetPointer(address,size),size};}
uint32_t Resource(uint32_t reference,bool required=false) {
    uint32_t data=0;Checked(mh3_res_runtime_reference(&MH3NativeResourceMemory(),reference,required,&data));return data;
}
uint32_t Canonical(uint32_t address) {return address?(address&0x3fffffffu)|0x80000000u:0;}
void Publish(const mh3_native_material_state& patch,uint32_t top=0) {
    auto next=GetNativeMaterialState();Checked(mh3_material_state_overlay(&next,&patch,top));SetNativeMaterial(next);
}
mh3_model_genmode Mode() {
    mh3_model_genmode mode{};Checked(mh3_model_genmode_read(Bytes(genmode,12),&mode));return mode;
}
void Genmode(bool xf) {
    mh3_native_material_state patch{};uint32_t top=0;int apply=0;
    Checked(mh3_model_genmode_publish(Mutable(genmode,12),xf,&patch,&top,&apply));if(apply) Publish(patch,top);
}
void Group(uint32_t address,mh3_model_material_block kind,bool xf=false) {
    if(!address) return;
    const auto mode=Mode();const uint32_t count=kind==MH3_MODEL_MATERIAL_INDIRECT?mode.indirect:
        kind==MH3_MODEL_MATERIAL_TEXGEN?mode.texgens:1;
    const auto patch=ReadNativeModelMaterialGroup(address,kind,count);
    if(!xf) Genmode(false);
    Memory::GetPointer(sync_address,1)[0]=0;
    if(count) Publish(patch);
    if(xf) Genmode(true);
}
mh3_model_material_scope Scope(uint32_t reference,uint32_t swaps) {
    const uint32_t material=Resource(reference);uint32_t version=0;
    if(material) {
        uint32_t parent=0;Checked(mh3_res_runtime_offset(&MH3NativeResourceMemory(),reference,
            int32_t(Memory::Read32(material+4)),4,&parent));
        const auto owner=ResolveNativeModelOwner(parent);(void)ResolveNativeModelResource(owner,material,true);
        mh3_catalog_model model{};PackChecked(mh3_model_catalog_model(owner.catalog,owner.catalog_model_index,&model));version=model.revision;
    }
    mh3_model_material_bindings replacements{};
    if(swaps) Checked(mh3_model_material_bindings_read(Bytes(swaps,56),&replacements));
    mh3_model_material_scope scope{};Checked(mh3_model_material_scope_read(&MH3NativeResourceMemory(),material,version,&replacements,&scope));return scope;
}
struct PrepareContext {mh3_native_light_slots lights{};};
void OperatorKnown(uint32_t address) {
    Checked(mh3_matrix_runtime_operator(&MH3NativeResourceMemory(),address,0x80088a5c,0x800888bc));
    const uint32_t table=Memory::Read32(address);
    if(Memory::Read32(table+8)!=0x80088aac) throw std::runtime_error("Custom indirect apply callback has no native producer");
}
void Indirect(uint32_t resource,uint32_t op) {
    if(resource) Group(resource,MH3_MODEL_MATERIAL_INDIRECT);
    if(!op) return;
    if(op!=operator_address || !current_operator.active)
        throw std::runtime_error("Indirect matrix operator has no completed native producer");
    OperatorKnown(op);
    mh3_native_material_state base{},patch{};
    if(resource) {
        const auto source=ReadNativeLiveMaterialBlock(resource,MH3_MODEL_MATERIAL_INDIRECT);
        std::memcpy(base.indirect_matrix,source.fields.indirect_matrix,sizeof base.indirect_matrix);
    }
    Checked(mh3_model_indirect_scope(&base,&current_operator,&patch));Publish(patch);
}
mh3_res_status PrepareStep(void* opaque,const mh3_model_material_step* step) {
    auto& context=*static_cast<PrepareContext*>(opaque);const uint32_t resource=step->resource_address;
    switch(step->action) {
    case MH3_MATERIAL_PREP_FOG: {
        mh3_model_material_misc misc{};Checked(mh3_model_material_misc_read(Bytes(resource,12),&misc));
        mh3_native_fog fog{};int apply=0;Checked(mh3_fog_state_select(Mutable(0x80685d60,1544),uint32_t(int32_t(misc.fog_reference)),&fog,&apply));
        if(apply) SetNativeFog(fog);break;
    }
    case MH3_MATERIAL_PREP_MISC: {
        mh3_model_material_misc misc{};Checked(mh3_model_material_misc_read(Bytes(resource,12),&misc));int apply=0;
        Checked(mh3_depth_compare_cache(Mutable(0x80796dc8,5),Mutable(sync_address,1),misc.depth_before_texture,&apply));
        if(apply) SetNativeDepthCompareBeforeTexture(misc.depth_before_texture);break;
    }
    case MH3_MATERIAL_PREP_PALETTES: SelectNativeModelPaletteObjects(0x806854a0,resource);break;
    case MH3_MATERIAL_PREP_TEXTURES: SelectNativeModelTextureObjects(0x80685380,resource);break;
    case MH3_MATERIAL_PREP_GENMODE: Checked(mh3_model_genmode_select(Mutable(genmode,12),Bytes(resource,8)));break;
    case MH3_MATERIAL_PREP_PIPELINE: LoadNativeModelPipeline(resource);break;
    case MH3_MATERIAL_PREP_PIXEL: Group(resource,MH3_MODEL_MATERIAL_PIXEL);break;
    case MH3_MATERIAL_PREP_COLORS: Group(resource,MH3_MODEL_MATERIAL_COLORS);break;
    case MH3_MATERIAL_PREP_INDIRECT: Indirect(resource,step->indirect_operator);break;
    case MH3_MATERIAL_PREP_LIGHTS: {
        mh3_model_material_misc misc{};Checked(mh3_model_material_misc_read(Bytes(resource,12),&misc));
        context.lights=SelectNativeModelLightSet(0x80686368,uint32_t(int32_t(misc.light_reference)));break;
    }
    case MH3_MATERIAL_PREP_CHANNELS: {
        const uint32_t masks[]{context.lights.diffuse_color,context.lights.diffuse_alpha,context.lights.specular_color,context.lights.specular_alpha};
        mh3_native_material_state patch{};Checked(mh3_model_channels_prepare(Bytes(resource,40),Mode().channels,masks,
            reinterpret_cast<const uint8_t*>(&context.lights.ambient),step->force_light_off,&patch));Publish(patch);Genmode(true);break;
    }
    case MH3_MATERIAL_PREP_TEXGEN: Group(resource,MH3_MODEL_MATERIAL_TEXGEN,true);break;
    case MH3_MATERIAL_PREP_SRT: LoadNativeModelTextureSrt(resource);break;
    }
    return MH3_RES_OK;
}
struct ShapeSources {
    uint64_t source_generation,resident_generation;
    uintptr_t resident_handle;
    std::array<mh3_native_vertex_source,12> source;
    uint16_t known;
};
std::unordered_map<uint32_t,ShapeSources> shape_sources;
const ShapeSources& Sources(const NativeModelOwner& owner,uint32_t reference,uint32_t shape) {
    const uint32_t key=Canonical(shape);const auto cached=shape_sources.find(key);
    if(cached!=shape_sources.end() && cached->second.source_generation==owner.source_generation &&
       cached->second.resident_handle==owner.resident_handle && cached->second.resident_generation==owner.resident_generation) return cached->second;
    ShapeSources result{};result.source_generation=owner.source_generation;result.resident_handle=owner.resident_handle;result.resident_generation=owner.resident_generation;
    for(uint32_t i=0;i<12;i++) {
        uint32_t resource=0;Checked(mh3_res_runtime_shape_array(&MH3NativeResourceMemory(),reference,i,&resource));if(!resource) continue;
        const auto kind=i==0?MH3_VERTEX_POSITION:i==1?MH3_VERTEX_NORMAL:i<4?MH3_VERTEX_COLOR:MH3_VERTEX_UV;
        result.source[i]=ReadNativeVertexResourceData(resource,kind);result.known|=uint16_t(1u<<i);
    }
    // Owned scalar metadata only. Bound the cache across recycled scene heaps;
    // evicting it requires fresh CPU header resolution, never GPU repacking.
    if(shape_sources.size()>=8192) shape_sources.clear();
    shape_sources.insert_or_assign(key,result);return shape_sources.at(key);
}
NativeModelOwner ShapeOwner(uint32_t reference,uint32_t* shape) {
    *shape=Resource(reference,true);uint32_t parent=0;Checked(mh3_res_runtime_shape_parent(&MH3NativeResourceMemory(),reference,&parent));
    auto owner=ResolveNativeModelOwner(parent);(void)ResolveNativeModelResource(owner,*shape,false);return owner;
}
void PrePrimitive(uint32_t reference) {
    if(!Resource(reference)) return;
    uint32_t shape=0;const auto owner=ShapeOwner(reference,&shape);const auto& sources=Sources(owner,reference,shape);
    PrepareNativeModelCoordinateSizes(Mode().texgens);Genmode(true);
    Checked(mh3_model_shape_cache(Mutable(0x80685504,12),Bytes(shape+12,12)));Memory::GetPointer(sync_address,1)[0]=0;
    for(uint32_t i=0;i<12;i++) if(sources.known&(1u<<i)) StoreNativeVertexSource(i,sources.source[i]);
}
void Replacements(uint32_t reference,const mh3_model_material_scope& scope) {
    uint32_t shape=0;const auto owner=ShapeOwner(reference,&shape);const auto& originals=Sources(owner,reference,shape);
    for(unsigned i=0;i<4;i++) {
        const uint32_t table=scope.selected.vertex_tables[i<2?i:2];if(!table) continue;
        const int32_t id=int16_t(Memory::Read16(shape+0x48+i*2));if(id<0) continue;
        if(uint64_t(table)+uint32_t(id)*4>UINT32_MAX) throw std::runtime_error("Model replacement vertex table overflows");
        const uint32_t resource=Memory::Read32(table+uint32_t(id)*4);
        const auto kind=i==0?MH3_VERTEX_POSITION:i==1?MH3_VERTEX_NORMAL:MH3_VERTEX_COLOR;
        const auto source=ReadNativeVertexResourceData(resource,kind);
        if(i==0 && Canonical(source.payload_address)!=Canonical(originals.source[0].payload_address))
            throw std::runtime_error("Position replacement differs from the shape's authored array");
        StoreNativeVertexSource(i,source);
    }
}
} // namespace

void PublishNativeModelMapSize(uint32_t map,uint16_t width,uint16_t height) {
    if(map>=8 || !width || !height) throw std::runtime_error("Native G3D map size is invalid");
    map_sizes.width[map]=width;map_sizes.height[map]=height;map_sizes.known|=uint8_t(1u<<map);
}
const mh3_model_texcoord_sizes& ReadNativeModelCoordinateSizes() {return coordinate_sizes;}
void PrepareNativeModelCoordinateSizes(uint32_t count) {
    mh3_model_texcoord_sizes sizes{};uint8_t written=0;int apply=0;
    Checked(mh3_model_coordinate_prepare(Mutable(coordinate_cache,108),count,coordinate_map_known,&map_sizes,&coordinate_sizes,&sizes,&written,&apply));
    if(apply) coordinate_sizes=sizes;
}
void LoadNativeModelPipeline(uint32_t resource) {
    if(!resource) return;
    if(resource>UINT32_MAX-32) throw std::runtime_error("Model pipeline offset overflows");
    const auto block=ReadNativeLiveMaterialBlock(resource+32,MH3_MODEL_MATERIAL_PIPELINE);
    if(Memory::Read32(0x80796dbc)==resource && pipeline_address==resource && pipeline_revision==block.fields.generation) return;
    uint8_t table[8],known=0;ReadNativeModelCoordinateMap(resource+32,table,&known);
    const auto patch=ReadNativeModelMaterialGroup(resource,MH3_MODEL_MATERIAL_PIPELINE);
    Checked(mh3_model_coordinate_table(Mutable(coordinate_cache,108),table));coordinate_map_known=known;
    Genmode(false);Memory::GetPointer(sync_address,1)[0]=0;Publish(patch);
    Memory::Write32(0x80796dbc,resource);pipeline_address=resource;pipeline_revision=block.fields.generation;
    PrepareNativeModelCoordinateSizes(Mode().texgens);
}
void PrepareNativeModelMaterial(uint32_t reference,uint32_t control,uint32_t swaps,uint32_t op,bool ignore) {
    const auto scope=Scope(reference,swaps);PrepareContext context{};
    Checked(mh3_model_material_prepare_scope(&scope,control,op,ignore,PrepareStep,&context));
}
void SubmitNativeModelShape(uint32_t reference,uint32_t direct_position,uint32_t direct_normal) {
    if(!Resource(reference)) return;
    uint32_t shape=0;const auto owner=ShapeOwner(reference,&shape);const auto& prepared=ReadNativePreparedMesh(owner,shape);
    const auto& originals=Sources(owner,reference,shape);
    for(uint32_t i=0;i<prepared.immutable.layout.count;i++) {
        const uint32_t semantic=prepared.immutable.layout.attributes[i].semantic;mh3_native_vertex_source selected{};
        if(!(originals.known&(1u<<semantic)) || !ReadNativeVertexSource(semantic,&selected) ||
           Canonical(selected.payload_address)!=Canonical(originals.source[semantic].payload_address) || selected.source_generation)
            throw std::runtime_error("Resident model vertex binding requires a converted authored source: "+std::to_string(semantic));
    }
    const auto binding=ReadNativeModelMatrixSources(locations.arrays);
    mh3_matrix_sources sources{};
    if(binding.address[0]) sources=ReadNativeModelPose(binding.address,owner.model_address);
    else if(!direct_position) throw std::runtime_error("Indexed model matrix has no completed pose producer");
    mh3_matrix_runtime_storage storage{};mh3_shape_matrix_input input{};mh3_matrix_sources ignored{};
    Checked(mh3_matrix_runtime_shape(&MH3NativeResourceMemory(),&locations,reference,direct_position,direct_normal,&storage,&input,&ignored));
    if(input.matrix_id<0) throw std::runtime_error("Envelope model requires converted skinning palette records");
    mh3_model_palette palette{};Checked(mh3_model_palette_rigid(&input,&sources,&palette));
    PrepareNativeModelCoordinateSizes(Mode().texgens);
    bool identity=true;for(uint32_t row:palette.texture_ids) if(row!=60) identity=false;
    if(!identity || !Memory::Read8(locations.current_valid)) PublishNativeModelMatrixRows(palette.texture_ids);
    Memory::Write8(locations.current_valid,identity?1:0);
    SetNativePositionMatrix(&palette.position[0].v[0][0]);
    if(palette.normal_valid&1) SetNativeNormalMatrix(0,&palette.normal[0].v[0][0]);
    for(uint32_t i=0;i<8;i++) if(palette.texture_valid&(1u<<i)) SetNativeTextureMatrix(30+i*3,&palette.texture[i].v[0][0],true);
    Genmode(true);Memory::GetPointer(sync_address,1)[0]=0;
    uint64_t revision=sources.generation;
    if(!revision) {if(direct_revision==UINT64_MAX) throw std::runtime_error("Direct model matrix revision overflow");revision=++direct_revision;}
    mh3_resolved_mesh_submit submit{};PackChecked(mh3_model_mesh_bind_pose(&prepared,owner.resident_generation,owner.source_generation,revision,&palette,&submit));
    auto material=GetNativeMaterialState();ApplyNativeModelSelectors(material);
    float post[8][12]{};uint8_t known=0,format=0;
    for(uint32_t i=0;i<8;i++) {bool matrix3x4=false;if(GetNativeTextureMatrix(64+i*3,post[i],&matrix3x4)) {known|=uint8_t(1u<<i);if(matrix3x4) format|=uint8_t(1u<<i);}}
    submit.material=&material;submit.projection=GetNativeDrawState().projection;submit.texture_matrices=post;
    submit.texture_matrix_known=known;submit.texture_matrix_3x4=format;submit.texcoord_sizes=&coordinate_sizes;
    SubmitNativeModelMesh(submit);
}

extern "C" void MH3_ModelMaterialScope(CpuContext* cpu) {
    PrepareNativeModelMaterial(cpu->gpr[3],cpu->gpr[4],cpu->gpr[5],cpu->gpr[6],cpu->gpr[7]!=0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80076220,MH3_ModelMaterialScope,"ModelMaterialScope");
extern "C" void MH3_ModelPipelineLoad(CpuContext* cpu) {LoadNativeModelPipeline(Resource(cpu->gpr[3]));}
REGISTER_NATIVE_FUNCTION_AS(0x800865CC,MH3_ModelPipelineLoad,"ModelPipelineLoad");
extern "C" void MH3_ModelIndirectOperatorLoad(CpuContext* cpu) {
    const uint32_t resource=Resource(cpu->gpr[3]);if(resource) Indirect(resource,cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80086B0C,MH3_ModelIndirectOperatorLoad,"ModelIndirectOperatorLoad");
extern "C" void MH3_ModelShapePrePrimitive(CpuContext* cpu) {PrePrimitive(cpu->gpr[3]);}
REGISTER_NATIVE_FUNCTION_AS(0x800879DC,MH3_ModelShapePrePrimitive,"ModelShapePrePrimitive");
extern "C" void MH3_ModelShapePrimitive(CpuContext* cpu) {SubmitNativeModelShape(cpu->gpr[3],cpu->gpr[4],cpu->gpr[5]);}
REGISTER_NATIVE_FUNCTION_AS(0x80087B34,MH3_ModelShapePrimitive,"ModelShapePrimitive");
extern "C" void MH3_ModelIndirectMatrices(CpuContext* cpu) {
    current_operator={};operator_address=0;
    const uint32_t material=Resource(cpu->gpr[3]),shape=Resource(cpu->gpr[5]);
    if(material && shape && (Memory::Read8(material+0x20) || Memory::Read8(material+0x21) || Memory::Read8(material+0x22))) {
        uint32_t parent=0;Checked(mh3_res_runtime_shape_parent(&MH3NativeResourceMemory(),cpu->gpr[5],&parent));
        const auto owner=ResolveNativeModelOwner(parent);const auto binding=ReadNativeModelMatrixSources(locations.arrays);
        if(int32_t(Memory::Read32(shape+8))>=0) (void)ReadNativeModelPose(binding.address,owner.model_address);
    }
    Checked(mh3_matrix_runtime_indirect(&MH3NativeResourceMemory(),&locations,cpu->gpr[3],cpu->gpr[4],cpu->gpr[5],&current_operator));
    if(!current_operator.active) {cpu->gpr[3]=0;return;}
    operator_address=Resource(0x80793708,true);OperatorKnown(operator_address);
    Checked(mh3_matrix_store_indirect(Mutable(operator_address,152),&current_operator));cpu->gpr[3]=operator_address;
}
REGISTER_NATIVE_FUNCTION_AS(0x80077814,MH3_ModelIndirectMatrices,"ModelIndirectMatrices");
extern "C" void MH3_ModelDispatch(CpuContext* cpu) {
    TraceNativeModelDispatch(cpu);
    const auto scope=Scope(cpu->gpr[3],cpu->gpr[8]);const bool ignore=!scope.material_address || (cpu->gpr[7]&16);
    PrepareContext context{};Checked(mh3_model_material_prepare_scope(&scope,cpu->gpr[7],cpu->gpr[9],ignore,PrepareStep,&context));
    uint32_t visible=0;Checked(mh3_res_runtime_shape_visible(&MH3NativeResourceMemory(),cpu->gpr[4],&visible));if(!visible) return;
    if(scope.fur_address) throw std::runtime_error("Fur model needs native sampled-shell material preparation");
    if(!(cpu->gpr[7]&2)) {PrePrimitive(cpu->gpr[4]);Replacements(cpu->gpr[4],scope);}
    if(cpu->gpr[7]&4) {
        auto material=GetNativeMaterialState();material.cull_mode=1;SetNativeMaterial(material);
        std::memset(Memory::GetPointer(0x80685504,12),0,12);Memory::Write8(sync_address,1);
    }
    SubmitNativeModelShape(cpu->gpr[4],cpu->gpr[5],cpu->gpr[6]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80076A58,MH3_ModelDispatch,"ModelDisplayListDispatch");
