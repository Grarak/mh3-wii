#include "mh3_model_submission.h"
#include <math.h>
#include <string.h>

mh3_pack_status mh3_model_material_prepare(const mh3_mesh_pack *pack,uint32_t index,mh3_prepared_material *out) {
    if(!pack || !out) return MH3_PACK_INVALID;
    mh3_prepared_material material;mh3_pack_native_material native;mh3_pack_material base;
    memset(&material,0,sizeof material);
    mh3_pack_status status=mh3_material_state_read(pack,index,&material.state);if(status!=MH3_PACK_OK) return status;
    status=mh3_mesh_pack_material_native(pack,index,&native);if(status!=MH3_PACK_OK) return status;
    status=mh3_mesh_pack_material(pack,index,&base);if(status!=MH3_PACK_OK) return status;
    if(base.binding_count>8) return MH3_PACK_UNSUPPORTED;
    material.srt.flags=native.srt_flags;material.srt.mode=native.texture_matrix_mode;
    for(uint32_t i=0;i<8;i++) {
        status=mh3_mesh_pack_texture_srt(&native,i,&material.srt.slot[i]);if(status!=MH3_PACK_OK) return status;
    }
    material.binding_count=base.binding_count;
    for(uint32_t i=0;i<base.binding_count;i++) {
        status=mh3_mesh_pack_binding(pack,base.binding_first+i,&material.bindings[i]);if(status!=MH3_PACK_OK) return status;
    }
    *out=material;return MH3_PACK_OK;
}

mh3_pack_status mh3_model_mesh_prepare(const mh3_model_catalog *catalog,const mh3_mesh_pack *pack,
                                      uint32_t model_index,uint32_t shape_id,uintptr_t handle,
                                      uint64_t resident_generation,mh3_prepared_mesh *out) {
    if (!out || !pack || !catalog || !handle || !resident_generation) return MH3_PACK_INVALID;
    mh3_catalog_resource resource;mh3_catalog_model model;mh3_resolved_mesh_submit r;
    mh3_pack_status status=mh3_model_catalog_model(catalog,model_index,&model);
    if (status!=MH3_PACK_OK) return status;
    status=mh3_model_catalog_find_resource(catalog,model_index,0,shape_id,&resource);
    if (status!=MH3_PACK_OK) return status;
    if (resource.pack_index==UINT32_MAX) return MH3_PACK_UNSUPPORTED;
    memset(&r,0,sizeof r);
    status=mh3_mesh_pack_mesh(pack,resource.pack_index,&r.mesh);if (status!=MH3_PACK_OK) return status;
    if (r.mesh.model_id!=model.model_id || r.mesh.source_id!=shape_id || r.mesh.asset_id!=resource.asset_id)
        return MH3_PACK_INVALID;
    if (r.mesh.matrix_id<0) return MH3_PACK_UNSUPPORTED;
    if (!r.mesh.vertices.data || !r.mesh.indices.data) return MH3_PACK_INVALID;
    status=mh3_mesh_pack_layout(pack,r.mesh.layout_index,&r.layout);if (status!=MH3_PACK_OK) return status;
    int position=0,normal=0;
    for (uint32_t i=0;i<r.layout.count;i++) {
        const mh3_pack_attribute a=r.layout.attributes[i];
        if (a.semantic>11 || (a.semantic==0 && (a.format!=9 || a.components!=3)) ||
            (a.semantic==1 && ((a.format!=9 && a.format!=7) || a.components!=3)) ||
            ((a.semantic==2 || a.semantic==3) && (a.format!=4 || a.components!=4)) ||
            (a.semantic>=4 && ((a.format!=9 && a.format!=8) || a.components>3))) return MH3_PACK_UNSUPPORTED;
        if (a.semantic==0) position=1;
        if (a.semantic==1) normal=1;
    }
    if (!position) return MH3_PACK_INVALID;
    r.pack=pack;r.resident_handle=handle;r.resident_generation=resident_generation;
    r.model_id=model.model_id;r.shape_id=shape_id;r.asset_id=resource.asset_id;
    r.mesh_index=resource.pack_index;r.index_count=r.mesh.index_count;
    mh3_prepared_mesh prepared;memset(&prepared,0,sizeof prepared);
    prepared.immutable=r;prepared.normal_required=(uint8_t)normal;
    *out=prepared;return MH3_PACK_OK;
}
mh3_pack_status mh3_model_mesh_bind_pose(const mh3_prepared_mesh *prepared,
                                        uint64_t resident_generation,uint64_t source_generation,
                                        uint64_t pose_generation,const mh3_model_palette *palette,
                                        mh3_resolved_mesh_submit *out) {
    if(!prepared || !out || !palette || !source_generation || !pose_generation ||
       resident_generation!=prepared->immutable.resident_generation || !(palette->position_valid&1) ||
       (prepared->normal_required && !(palette->normal_valid&1))) return MH3_PACK_INVALID;
    for (uint32_t row=0;row<3;row++) for (uint32_t col=0;col<4;col++)
        if (!isfinite(palette->position[0].v[row][col])) return MH3_PACK_INVALID;
    if (palette->normal_valid&1) for (uint32_t row=0;row<3;row++) for (uint32_t col=0;col<3;col++)
        if (!isfinite(palette->normal[0].v[row][col])) return MH3_PACK_INVALID;
    mh3_resolved_mesh_submit r=prepared->immutable;
    r.source_generation=source_generation;r.pose_generation=pose_generation;r.palette=palette;
    *out=r;return MH3_PACK_OK;
}
mh3_pack_status mh3_model_mesh_resolve(const mh3_model_catalog *catalog,const mh3_mesh_pack *pack,
                                      uint32_t model_index,uint32_t shape_id,uintptr_t handle,
                                      uint64_t resident_generation,uint64_t source_generation,
                                      uint64_t pose_generation,const mh3_model_palette *palette,
                                      mh3_resolved_mesh_submit *out) {
    mh3_prepared_mesh prepared;
    mh3_pack_status status=mh3_model_mesh_prepare(catalog,pack,model_index,shape_id,handle,resident_generation,&prepared);
    if(status!=MH3_PACK_OK) return status;
    return mh3_model_mesh_bind_pose(&prepared,resident_generation,source_generation,pose_generation,palette,out);
}
