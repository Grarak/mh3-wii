#include "mh3_model_submission.h"
#include <math.h>
#include <string.h>

mh3_res_status mh3_model_texcoord_sizes_select(const mh3_prepared_material *prepared,uint32_t count,
    const mh3_model_texcoord_sizes *maps,const mh3_model_texcoord_sizes *current,
    mh3_model_texcoord_sizes *out,uint8_t *written) {
    if(!prepared || !maps || !current || !out || !written || count>8) return MH3_RES_INVALID;
    mh3_model_texcoord_sizes result=*current;uint8_t changed=0;
    for(uint32_t i=0;i<count;i++) {
        if(!(prepared->texcoord_map_known&(1u<<i))) return MH3_RES_UNSUPPORTED;
        const uint32_t map=prepared->texcoord_map[i];
        if(map==255) continue;
        if(map>7) return MH3_RES_INVALID;
        if(!(maps->known&(1u<<map))) return MH3_RES_ABSENT;
        if(!maps->width[map] || !maps->height[map]) return MH3_RES_INVALID;
        result.width[i]=maps->width[map];result.height[i]=maps->height[map];
        changed|=(uint8_t)(1u<<i);result.known|=(uint8_t)(1u<<i);
    }
    *out=result;*written=changed;return MH3_RES_OK;
}

mh3_pack_status mh3_model_material_group_select(const mh3_prepared_material *prepared,
    mh3_model_material_block block,uint32_t count,mh3_native_material_state *out) {
    if(!prepared) return MH3_PACK_INVALID;
    return mh3_model_material_fields_select(&prepared->state,block,count,out);
}
mh3_pack_status mh3_model_material_fields_select(const mh3_native_material_state *source,
    mh3_model_material_block block,uint32_t count,mh3_native_material_state *out) {
    if(!source || !out || block<0 || block>=MH3_MODEL_MATERIAL_BLOCK_COUNT)
        return MH3_PACK_INVALID;
    if(source->tev_count>16 || source->texgen_count>8 || source->indirect_count>4)
        return MH3_PACK_INVALID;
    if(block==MH3_MODEL_MATERIAL_TEXGEN && count && count!=source->texgen_count)
        return MH3_PACK_UNSUPPORTED;
    if(block==MH3_MODEL_MATERIAL_PIPELINE && !source->tev_count)
        return MH3_PACK_UNSUPPORTED;
    mh3_native_material_state selected;memset(&selected,0,sizeof selected);
    switch(block) {
    case MH3_MODEL_MATERIAL_PIXEL:
        selected.pixel=source->pixel;
        selected.pixel.known&=(UINT64_C(1)<<MH3_PIXEL_DEPTH_TEXTURE_OPERATION)-1;
        for(uint32_t i=MH3_PIXEL_DEPTH_TEXTURE_OPERATION;i<MH3_PIXEL_FIELD_COUNT;i++) selected.pixel.values[i]=0;
        break;
    case MH3_MODEL_MATERIAL_COLORS:
        memcpy(selected.color,source->color,sizeof selected.color);
        memcpy(selected.konst,source->konst,sizeof selected.konst);
        break;
    case MH3_MODEL_MATERIAL_INDIRECT:
        if(count) {
            memcpy(selected.indirect_matrix,source->indirect_matrix,sizeof selected.indirect_matrix);
            for(uint32_t i=0;i<4;i++) {
                selected.indirect[i].known=source->indirect[i].known&12u;
                if(selected.indirect[i].known&4u) selected.indirect[i].scale_s=source->indirect[i].scale_s;
                if(selected.indirect[i].known&8u) selected.indirect[i].scale_t=source->indirect[i].scale_t;
            }
        }
        break;
    case MH3_MODEL_MATERIAL_TEXGEN:
        if(count) for(uint32_t i=0;i<8;i++) {
            selected.texgen[i]=source->texgen[i];
            selected.texgen[i].known&=~((1u<<MH3_TEXGEN_MATRIX_ROW)|(1u<<MH3_TEXGEN_DUAL_TEXTURE));
            selected.texgen[i].values[MH3_TEXGEN_MATRIX_ROW]=0;
            selected.texgen[i].values[MH3_TEXGEN_DUAL_TEXTURE]=0;
        }
        break;
    case MH3_MODEL_MATERIAL_PIPELINE:
        memcpy(selected.tev,source->tev,sizeof selected.tev);
        memcpy(selected.swaps,source->swaps,sizeof selected.swaps);
        for(uint32_t i=0;i<4;i++) {
            selected.indirect[i].known=source->indirect[i].known&3u;
            if(selected.indirect[i].known&1u) selected.indirect[i].coordinate=source->indirect[i].coordinate;
            if(selected.indirect[i].known&2u) selected.indirect[i].map=source->indirect[i].map;
        }
        break;
    default:return MH3_PACK_INVALID;
    }
    *out=selected;return MH3_PACK_OK;
}

mh3_pack_status mh3_model_material_prepare(const mh3_mesh_pack *pack,uint32_t index,mh3_prepared_material *out) {
    if(!pack || !out) return MH3_PACK_INVALID;
    mh3_prepared_material material;mh3_pack_native_material native;mh3_pack_material base;
    memset(&material,0,sizeof material);
    mh3_pack_status status=mh3_material_state_read(pack,index,&material.state);if(status!=MH3_PACK_OK) return status;
    status=mh3_mesh_pack_material_native(pack,index,&native);if(status!=MH3_PACK_OK) return status;
    status=mh3_mesh_pack_material(pack,index,&base);if(status!=MH3_PACK_OK) return status;
    status=mh3_mesh_pack_texcoord_map(&native,material.texcoord_map,&material.texcoord_map_known);
    if(status!=MH3_PACK_OK && status!=MH3_PACK_UNSUPPORTED) return status;
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
    const int indexed=(r.mesh.flags&MH3_MESH_INDEXED_PALETTE)!=0;
    if (r.mesh.matrix_id<0 && !indexed) return MH3_PACK_UNSUPPORTED;
    /* Metadata admission is independent of GPU residency. A demand loader
     * can prepare immutable scalar layouts/ranges before allocating CDRAM.
     * The backend later replaces spans with its validated resident mesh. */
    if ((!r.mesh.vertices.data || !r.mesh.indices.data) &&
        (r.mesh.vertices.data || r.mesh.indices.data || pack->gpu.data || pack->gpu.size)) return MH3_PACK_INVALID;
    status=mh3_mesh_pack_layout(pack,r.mesh.layout_index,&r.layout);if (status!=MH3_PACK_OK) return status;
    int position=0,normal=0;
    for (uint32_t i=0;i<r.layout.count;i++) {
        const mh3_pack_attribute a=r.layout.attributes[i];
        if (a.semantic>14 || (a.semantic>=12 && (!indexed || a.format!=0 || a.components!=(a.semantic==14?1:4))) ||
            (a.semantic==0 && (a.format!=9 || a.components!=3)) ||
            (a.semantic==1 && ((a.format!=9 && a.format!=7) || a.components!=3)) ||
            ((a.semantic==2 || a.semantic==3) && (a.format!=4 || a.components!=4)) ||
            (a.semantic>=4 && a.semantic<=11 && ((a.format!=9 && a.format!=8) || a.components>3))) return MH3_PACK_UNSUPPORTED;
        if (a.semantic==0) position=1;
        if (a.semantic==1) normal=1;
    }
    if (!position) return MH3_PACK_INVALID;
    r.pack=pack;r.resident_handle=handle;r.resident_generation=resident_generation;
    r.model_id=model.model_id;r.shape_id=shape_id;r.asset_id=resource.asset_id;
    r.mesh_index=resource.pack_index;r.index_count=r.mesh.index_count;
    /* CPU admission retains scalar ranges only. GPU-only eviction/reload must
     * never leave borrowed allocation pointers in the catalogue cache. The
     * backend resolves its current resident mesh after frame admission. */
    r.mesh.vertices=(mh3_pack_span){NULL,0};r.mesh.indices=(mh3_pack_span){NULL,0};
    mh3_prepared_mesh prepared;memset(&prepared,0,sizeof prepared);
    prepared.immutable=r;prepared.normal_required=(uint8_t)normal;
    prepared.matrix_index_mask=(uint16_t)((r.layout.flags>>MH3_LAYOUT_MATRIX_MASK_SHIFT)&511u);
    if(indexed) {
        mh3_pack_palette_batches batches;
        status=mh3_mesh_pack_palette_batches(pack,r.mesh_index,&batches);if(status!=MH3_PACK_OK) return status;
        prepared.palette_batch_count=batches.count;
    }
    *out=prepared;return MH3_PACK_OK;
}
mh3_pack_status mh3_model_mesh_bind_pose(const mh3_prepared_mesh *prepared,
                                        uint64_t resident_generation,uint64_t source_generation,
                                        uint64_t pose_generation,const mh3_model_palette *palette,
                                        mh3_resolved_mesh_submit *out) {
    if(prepared && prepared->palette_batch_count) return MH3_PACK_UNSUPPORTED;
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
mh3_pack_status mh3_model_mesh_bind_batch(const mh3_prepared_mesh *prepared,uint32_t index,
                                         uint64_t resident_generation,uint64_t source_generation,
                                         uint64_t palette_generation,const mh3_model_palette *palette,
                                         mh3_resolved_mesh_submit *out) {
    if(!prepared || !palette || !out || !prepared->palette_batches || index>=prepared->palette_batch_count ||
       !source_generation || !palette_generation || resident_generation!=prepared->immutable.resident_generation)
        return MH3_PACK_INVALID;
    const mh3_pack_palette_batch *batch=&prepared->palette_batches[index];
    uint16_t positions=batch->position_required,normals=batch->normal_required;
    if(!(prepared->matrix_index_mask&1u)) {positions|=1;if(prepared->normal_required) normals|=1;}
    if(((positions|normals|batch->texture_required)&~1023u) ||
       (positions&~palette->position_valid) || (normals&~palette->normal_valid) ||
       (batch->texture_required&~palette->texture_valid)) return MH3_PACK_INVALID;
    for(unsigned i=0;i<10;i++) {
        if(positions&(1u<<i)) for(unsigned r=0;r<3;r++) for(unsigned c=0;c<4;c++)
            if(!isfinite(palette->position[i].v[r][c])) return MH3_PACK_INVALID;
        if(normals&(1u<<i)) for(unsigned r=0;r<3;r++) for(unsigned c=0;c<3;c++)
            if(!isfinite(palette->normal[i].v[r][c])) return MH3_PACK_INVALID;
        if(batch->texture_required&(1u<<i)) for(unsigned r=0;r<3;r++) for(unsigned c=0;c<4;c++)
            if(!isfinite(palette->texture[i].v[r][c])) return MH3_PACK_INVALID;
    }
    mh3_resolved_mesh_submit result=prepared->immutable;
    result.first_index=batch->first_index;result.index_count=batch->index_count;result.palette_batch=batch;
    result.palette=palette;result.pose_generation=palette_generation;result.source_generation=source_generation;
    *out=result;return MH3_PACK_OK;
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
