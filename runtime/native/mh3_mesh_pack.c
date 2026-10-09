#include "mh3_mesh_pack.h"
#include <float.h>
#include <math.h>
#include <string.h>

_Static_assert(sizeof(float)==4, "M3MP requires 32-bit IEEE float");
_Static_assert(FLT_RADIX==2 && FLT_MANT_DIG==24 && FLT_MAX_EXP==128, "M3MP requires IEEE binary32");
static const uint32_t record_sizes[8]={128,192,160,128,128,64,32,64};
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0]|(uint16_t)p[1]<<8); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static int32_t signed_word(uint32_t v) {
    return (int32_t)((v&UINT32_C(0x80000000))?(int64_t)v-INT64_C(0x100000000):(int64_t)v);
}
static float lefloat(const uint8_t *p) {
    uint32_t word=le32(p); float value; memcpy(&value,&word,4); return value;
}
static int fits(mh3_pack_span s,size_t at,size_t n) {
    return s.data && at<=s.size && n<=s.size-at;
}
static int blob_fits(const mh3_mesh_pack *p,uint32_t at,uint64_t n) {
    return at>=p->data_offset && at<=p->metadata.size && n<=p->metadata.size-at;
}
static int finite_values(const float *values,unsigned count) {
    unsigned i; for (i=0;i<count;++i) if (!isfinite(values[i])) return 0;
    return 1;
}
static int valid_bounds(const float low[3],const float high[3],const float center[3]) {
    unsigned i;
    if (!finite_values(low,3) || !finite_values(high,3) || !finite_values(center,3)) return 0;
    for (i=0;i<3;++i) if (low[i]>high[i] || center[i]<low[i] || center[i]>high[i]) return 0;
    return 1;
}
static int range(uint32_t first,uint32_t count,uint32_t total) {
    return first<=total && count<=total-first;
}
static const uint8_t *record(const mh3_mesh_pack *p,mh3_pack_table t,uint32_t i) {
    size_t at;
    if (!p || (unsigned)t>=8 || i>=p->counts[t]) return NULL;
    at=p->offsets[t]+(size_t)i*record_sizes[t];
    return fits(p->metadata,at,record_sizes[t])?p->metadata.data+at:NULL;
}
static const char *string_at(const mh3_mesh_pack *p,uint32_t at) {
    if (!at || !blob_fits(p,at,1)) return NULL;
    return memchr(p->metadata.data+at,0,p->metadata.size-at)?(const char *)(p->metadata.data+at):NULL;
}
static void floats(float *out,const uint8_t *at,unsigned count) {
    unsigned i; for (i=0;i<count;++i) out[i]=lefloat(at+i*4);
}

mh3_pack_status mh3_mesh_pack_layout(const mh3_mesh_pack *p,uint32_t i,mh3_pack_layout *out) {
    const uint8_t *r=record(p,MH3_PACK_LAYOUTS,i);
    uint32_t a,seen=0;
    if (!out) return MH3_PACK_INVALID;
    memset(out,0,sizeof(*out));
    if (!r) return MH3_PACK_RANGE;
    out->count=le32(r); out->stride=le32(r+4); out->flags=le32(r+8);
    if (!out->count || out->count>16 || !out->stride || out->stride>65535 || out->flags>1 || le32(r+12)) return MH3_PACK_INVALID;
    for (a=0;a<out->count;++a) {
        const uint8_t *v=r+16+a*8;
        uint32_t bytes;
        mh3_pack_attribute *attr=&out->attributes[a];
        attr->semantic=le16(v); attr->offset=le16(v+2); attr->format=v[4]; attr->components=v[5];
        if (attr->semantic>=12 || (seen&(1u<<attr->semantic)) || !attr->components || attr->components>4 || le16(v+6)) return MH3_PACK_INVALID;
        seen|=1u<<attr->semantic;
        if (attr->format==9) bytes=4*attr->components;
        else if (attr->format==4) bytes=attr->components;
        else if (attr->format==7 || attr->format==8) bytes=2*attr->components;
        else return MH3_PACK_UNSUPPORTED;
        if (attr->offset>out->stride || bytes>out->stride-attr->offset) return MH3_PACK_RANGE;
    }
    if (!(seen&1)) return MH3_PACK_INVALID;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_mesh(const mh3_mesh_pack *p,uint32_t i,mh3_pack_mesh *out) {
    const uint8_t *r=record(p,MH3_PACK_MESHES,i);
    mh3_pack_layout layout;
    mh3_pack_status s;
    if (!out) return MH3_PACK_INVALID;
    memset(out,0,sizeof(*out));
    if (!r) return MH3_PACK_RANGE;
    out->asset_id=le32(r); out->model_id=le32(r+4); out->source_id=le32(r+8); out->flags=le32(r+12);
    out->layout_index=le32(r+16); out->vertex_count=le32(r+20); out->index_count=le32(r+24);
    out->vertex_offset=le32(r+28); out->vertex_bytes=le32(r+32); out->index_offset=le32(r+36); out->index_bytes=le32(r+40);
    out->matrix_id=signed_word(le32(r+44)); out->component_first=le32(r+48); out->component_count=le32(r+52);
    out->template_offset=le32(r+56); out->template_indices=le32(r+60);
    floats(out->bounds_min,r+64,3); floats(out->bounds_max,r+76,3); floats(out->center,r+88,3);
    if (!out->asset_id || !out->model_id || (out->flags&~7u) || !(out->flags&1) || out->matrix_id<0 ||
        !out->vertex_count || out->vertex_count>65536 || !out->index_count || out->index_count%3 ||
        !valid_bounds(out->bounds_min,out->bounds_max,out->center)) return MH3_PACK_INVALID;
    s=mh3_mesh_pack_layout(p,out->layout_index,&layout); if (s) return s;
    if ((uint64_t)out->vertex_count*layout.stride!=out->vertex_bytes || (uint64_t)out->index_count*2!=out->index_bytes ||
        (out->vertex_offset&63) || (out->index_offset&63) || p->gpu_bytes<3 || !range(out->vertex_offset,out->vertex_bytes,p->gpu_bytes-3) ||
        !range(out->index_offset,out->index_bytes,p->gpu_bytes)) return MH3_PACK_RANGE;
    if (!range(out->component_first,out->component_count,p->counts[MH3_PACK_COMPONENTS])) return MH3_PACK_RANGE;
    if (out->component_count) {
        if (!(out->flags&2) || out->template_indices!=out->index_count ||
            !blob_fits(p,out->template_offset,out->index_bytes)) return MH3_PACK_RANGE;
    } else if (out->flags&2 || out->template_offset || out->template_indices) return MH3_PACK_INVALID;
    if (p->gpu.data) {
        out->vertices.data=p->gpu.data+out->vertex_offset; out->vertices.size=out->vertex_bytes;
        out->indices.data=p->gpu.data+out->index_offset; out->indices.size=out->index_bytes;
    }
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_component(const mh3_mesh_pack *p,uint32_t i,mh3_pack_component *out) {
    const uint8_t *r=record(p,MH3_PACK_COMPONENTS,i);
    if (!out) return MH3_PACK_INVALID;
    memset(out,0,sizeof(*out)); if (!r) return MH3_PACK_RANGE;
    out->mesh_index=le32(r); out->first_index=le32(r+4); out->index_count=le32(r+8); out->flags=le32(r+12);
    floats(out->bounds_min,r+16,3); floats(out->bounds_max,r+28,3); floats(out->centroid,r+40,3);
    if (out->mesh_index>=p->counts[MH3_PACK_MESHES] || !out->index_count || out->index_count%3 || out->flags>1 ||
        !valid_bounds(out->bounds_min,out->bounds_max,out->centroid)) return MH3_PACK_INVALID;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_draw(const mh3_mesh_pack *p,uint32_t i,mh3_pack_draw *out) {
    const uint8_t *r=record(p,MH3_PACK_DRAWS,i);
    if (!out) return MH3_PACK_INVALID;
    memset(out,0,sizeof(*out)); if (!r) return MH3_PACK_RANGE;
    out->mesh_index=le32(r); out->material_index=le32(r+4); out->node_index=le32(r+8); out->phase=le32(r+12);
    out->priority=le32(r+16); out->source_order=le32(r+20); out->asset_id=le32(r+24); out->flags=le32(r+28);
    if (out->mesh_index>=p->counts[MH3_PACK_MESHES] || out->material_index>=p->counts[MH3_PACK_MATERIALS] ||
        out->node_index>=p->counts[MH3_PACK_NODES] || out->phase>1 || out->priority>255 || out->flags&~7u) return MH3_PACK_RANGE;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_node(const mh3_mesh_pack *p,uint32_t i,mh3_pack_node *out) {
    const uint8_t *r=record(p,MH3_PACK_NODES,i);
    if (!out) return MH3_PACK_INVALID;
    memset(out,0,sizeof(*out)); if (!r) return MH3_PACK_RANGE;
    out->model_id=le32(r); out->source_id=le32(r+4); out->parent_index=signed_word(le32(r+8)); out->matrix_id=le32(r+12);
    out->flags=le32(r+16); out->billboard_mode=le32(r+20); out->first_child_index=signed_word(le32(r+24));
    floats(out->bind_model_matrix,r+32,12); floats(out->inverse_bind_model_matrix,r+80,12);
    floats(out->srt,r+128,9); floats(out->bounds,r+164,6);
    if (!out->model_id || out->billboard_mode>6 || out->parent_index < -1 || out->first_child_index < -1 ||
        (out->parent_index>=0 && (uint32_t)out->parent_index>=p->counts[MH3_PACK_NODES]) ||
        (out->first_child_index>=0 && (uint32_t)out->first_child_index>=p->counts[MH3_PACK_NODES]) ||
        !finite_values(out->bind_model_matrix,12) || !finite_values(out->inverse_bind_model_matrix,12) ||
        !finite_values(out->srt,9) || !finite_values(out->bounds,6)) return MH3_PACK_RANGE;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_material(const mh3_mesh_pack *p,uint32_t i,mh3_pack_material *out) {
    const uint8_t *r=record(p,MH3_PACK_MATERIALS,i);
    uint32_t w;
    if (!out) return MH3_PACK_INVALID;
    memset(out,0,sizeof(*out)); if (!r) return MH3_PACK_RANGE;
    out->asset_id=le32(r); out->model_id=le32(r+4); out->source_id=le32(r+8); out->flags=le32(r+12);
    out->name=string_at(p,le32(r+16)); out->cull_mode=le32(r+20); out->texgen_count=le32(r+24); out->channel_count=le32(r+28);
    out->tev_count=le32(r+32); out->indirect_count=le32(r+36); out->depth_compare_before_texture=le32(r+40);
    out->alpha_compare=le32(r+44); out->depth_state=le32(r+48); out->blend_state=le32(r+52); out->blend_mask=le32(r+56);
    out->state_offset=le32(r+60); out->state_count=le32(r+64); out->binding_first=le32(r+68); out->binding_count=le32(r+72);
    out->shader_pipeline_id=le32(r+80);
    if (!out->name || !out->asset_id || !out->model_id || out->flags&~3u || out->cull_mode>3 || out->texgen_count>8 || out->channel_count>2 ||
        out->tev_count>16 || out->indirect_count>4 || out->depth_compare_before_texture>1 ||
        !range(out->binding_first,out->binding_count,p->counts[MH3_PACK_BINDINGS]) ||
        !blob_fits(p,out->state_offset,(uint64_t)out->state_count*16)) return MH3_PACK_INVALID;
    for (w=0;w<out->state_count;++w) {
        const uint8_t *v=p->metadata.data+out->state_offset+(size_t)w*16;
        uint32_t bank=le32(v),addr=le32(v+4),mask=le32(v+12);
        if (bank>2 || (bank!=1 && addr>255) || (bank==1 && addr>65535) ||
            (bank==0 && (mask>0xffffff || le32(v+8)>0xffffff)) || (bank!=0 && mask!=UINT32_MAX)) return MH3_PACK_INVALID;
    }
    if (le32(p->metadata.data+4)==2) {
        mh3_pack_native_material native;
        mh3_pack_status status=mh3_mesh_pack_material_native(p,i,&native);
        if (status) return status;
    } else {
        for (w=84;w<128;w+=4) if (le32(r+w)) return MH3_PACK_UNSUPPORTED;
    }
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_model(const mh3_mesh_pack *p,uint32_t i,mh3_pack_model *out) {
    const uint8_t *r=record(p,MH3_PACK_MODELS,i);
    uint32_t offset,size;
    if (!out) return MH3_PACK_INVALID;
    memset(out,0,sizeof(*out)); if (!r) return MH3_PACK_RANGE;
    out->id=le32(r); out->name=string_at(p,le32(r+4)); out->file=string_at(p,le32(r+8)); out->source_offset=le32(r+12);
    out->node_first=le32(r+16); out->node_count=le32(r+20); out->mesh_first=le32(r+24); out->mesh_count=le32(r+28);
    out->material_first=le32(r+32); out->material_count=le32(r+36); out->draw_first=le32(r+40); out->draw_count=le32(r+44);
    offset=le32(r+48); size=le32(r+52); out->revision=le32(r+56); out->flags=le32(r+60);
    out->scaling_rule=le32(r+64); out->texture_matrix_mode=le32(r+68); out->view_matrix_count=le32(r+72);
    out->needs_normal_matrix_array=le32(r+76); out->needs_texture_matrix_array=le32(r+80);
    out->envelope_matrix_mode=le32(r+84); out->bounds_valid=le32(r+88);
    floats(out->declared_bounds,r+92,6); out->source_vertex_count=le32(r+116); out->source_triangle_count=le32(r+120);
    if (!out->id || !out->name || !out->file || !range(out->node_first,out->node_count,p->counts[MH3_PACK_NODES]) ||
        !range(out->mesh_first,out->mesh_count,p->counts[MH3_PACK_MESHES]) ||
        !range(out->material_first,out->material_count,p->counts[MH3_PACK_MATERIALS]) ||
        !range(out->draw_first,out->draw_count,p->counts[MH3_PACK_DRAWS]) || !blob_fits(p,offset,size) || size<16 ||
        out->id>65535 || out->flags&~1u || (out->revision!=9 && out->revision!=11) || out->needs_normal_matrix_array>1 ||
        out->needs_texture_matrix_array>1 || out->bounds_valid>1 || !finite_values(out->declared_bounds,6) || le32(r+124)) return MH3_PACK_RANGE;
    out->node_recipes.data=p->metadata.data+offset; out->node_recipes.size=size;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_binding(const mh3_mesh_pack *p,uint32_t i,mh3_pack_binding *out) {
    const uint8_t *r=record(p,MH3_PACK_BINDINGS,i);
    if (!out) return MH3_PACK_INVALID;
    memset(out,0,sizeof(*out)); if (!r) return MH3_PACK_RANGE;
    out->material_index=le32(r); out->texture_id=le32(r+4); out->map_id=le32(r+8); out->tlut_id=le32(r+12);
    out->wrap_s=le32(r+16); out->wrap_t=le32(r+20); out->min_filter=le32(r+24); out->mag_filter=le32(r+28);
    out->lod_bias=lefloat(r+32); out->anisotropy=le32(r+36); out->bias_clamp=le32(r+40); out->edge_lod=le32(r+44);
    out->texture_name=string_at(p,le32(r+48)); out->palette_name=string_at(p,le32(r+52)); out->flags=le32(r+56);
    if (out->material_index>=p->counts[MH3_PACK_MATERIALS] || out->map_id>7 || out->tlut_id>19 ||
        out->wrap_s>2 || out->wrap_t>2 || out->min_filter>5 || out->mag_filter>1 || out->anisotropy>2 ||
        out->bias_clamp>1 || out->edge_lod>1 || !isfinite(out->lod_bias) || !out->texture_name ||
        (le32(r+52) && !out->palette_name) || out->flags>1 || (!out->texture_id)!=(out->flags==1) || le32(r+60)) return MH3_PACK_INVALID;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_state_write(const mh3_mesh_pack *p,uint32_t mat_index,uint32_t i,mh3_pack_state_write *out) {
    const uint8_t *mat=record(p,MH3_PACK_MATERIALS,mat_index),*r;
    uint32_t offset,count;
    if (!out) return MH3_PACK_INVALID;
    memset(out,0,sizeof(*out)); if (!mat) return MH3_PACK_RANGE;
    offset=le32(mat+60); count=le32(mat+64);
    if (i>=count || !blob_fits(p,offset,(uint64_t)count*16)) return MH3_PACK_RANGE;
    r=p->metadata.data+offset+(size_t)i*16;
    out->bank=le32(r); out->address=le32(r+4); out->value=le32(r+8); out->mask=le32(r+12);
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_recipes(const mh3_mesh_pack *p,uint32_t i,mh3_pack_recipes *out) {
    mh3_pack_model model; uint64_t bytes; mh3_pack_status s;
    if (!out) return MH3_PACK_INVALID;
    memset(out,0,sizeof(*out)); s=mh3_mesh_pack_model(p,i,&model); if (s) return s;
    out->bytes=model.node_recipes;
    out->command_count=le32(out->bytes.data); out->weight_count=le32(out->bytes.data+4); out->matrix_count=le32(out->bytes.data+8);
    bytes=16+(uint64_t)out->command_count*24+(uint64_t)out->weight_count*8+(uint64_t)out->matrix_count*4;
    if (bytes!=out->bytes.size || le32(out->bytes.data+12)) return MH3_PACK_INVALID;
    return MH3_PACK_OK;
}
mh3_pack_status mh3_mesh_pack_recipe(const mh3_pack_recipes *p,uint32_t i,mh3_pack_recipe *out) {
    const uint8_t *r; uint64_t at=16+(uint64_t)i*24;
    if (!out) return MH3_PACK_INVALID;
    memset(out,0,sizeof(*out));
    if (!p || i>=p->command_count || at>SIZE_MAX || !fits(p->bytes,(size_t)at,24)) return MH3_PACK_RANGE;
    r=p->bytes.data+(size_t)at;
    out->opcode=le32(r); out->destination_or_node=le32(r+4); out->source_or_parent=le32(r+8);
    out->weight_first=le32(r+12); out->weight_count=le32(r+16);
    if (le32(r+20) || (out->opcode!=2 && out->opcode!=3 && out->opcode!=5 && out->opcode!=6)) return MH3_PACK_INVALID;
    if (out->opcode==3) {
        if (out->source_or_parent || !out->weight_count || !range(out->weight_first,out->weight_count,p->weight_count)) return MH3_PACK_RANGE;
    } else if (out->weight_first || out->weight_count) return MH3_PACK_INVALID;
    return MH3_PACK_OK;
}
mh3_pack_status mh3_mesh_pack_weight(const mh3_pack_recipes *p,uint32_t i,mh3_pack_weight *out) {
    uint64_t at; const uint8_t *r;
    if (!out) return MH3_PACK_INVALID;
    memset(out,0,sizeof(*out)); if (!p || i>=p->weight_count) return MH3_PACK_RANGE;
    at=16+(uint64_t)p->command_count*24+(uint64_t)i*8;
    if (at>SIZE_MAX || !fits(p->bytes,(size_t)at,8)) return MH3_PACK_RANGE;
    r=p->bytes.data+(size_t)at; out->matrix_id=le32(r); out->weight=lefloat(r+4);
    return out->matrix_id<p->matrix_count && isfinite(out->weight)?MH3_PACK_OK:MH3_PACK_INVALID;
}
mh3_pack_status mh3_mesh_pack_matrix_node(const mh3_pack_recipes *p,uint32_t i,int32_t *out) {
    uint64_t at;
    if (!out) return MH3_PACK_INVALID;
    *out=-1; if (!p || i>=p->matrix_count) return MH3_PACK_RANGE;
    at=16+(uint64_t)p->command_count*24+(uint64_t)p->weight_count*8+(uint64_t)i*4;
    if (at>SIZE_MAX || !fits(p->bytes,(size_t)at,4)) return MH3_PACK_RANGE;
    *out=signed_word(le32(p->bytes.data+(size_t)at)); return MH3_PACK_OK;
}

static mh3_pack_status open_metadata_validated(mh3_pack_span metadata,mh3_mesh_pack *out) {
    uint32_t i,t,bytes,previous=128,component_total=0,binding_total=0;
    uint32_t model_ends[4]={0,0,0,0};
    mh3_pack_status s;
    if (!out) return MH3_PACK_INVALID;
    memset(out,0,sizeof(*out));
    if (!fits(metadata,0,128)) return MH3_PACK_RANGE;
    if (memcmp(metadata.data,"M3MP",4)) return MH3_PACK_INVALID;
    if (le32(metadata.data+4)!=1 && le32(metadata.data+4)!=2) return MH3_PACK_UNSUPPORTED;
    bytes=le32(metadata.data+8);
    if (bytes<128 || bytes>metadata.size || bytes&63) return MH3_PACK_RANGE;
    out->metadata=metadata; out->metadata.size=bytes;
    out->gpu_file_offset=le32(metadata.data+12); out->gpu_bytes=le32(metadata.data+16);
    if (out->gpu_file_offset<bytes || out->gpu_file_offset&4095 || out->gpu_bytes&63 || out->gpu_bytes<64) return MH3_PACK_ALIGNMENT;
    for (t=0;t<8;++t) {
        uint64_t end;
        out->counts[t]=le32(metadata.data+20+t*8); out->offsets[t]=le32(metadata.data+24+t*8);
        end=(uint64_t)out->offsets[t]+(uint64_t)out->counts[t]*record_sizes[t];
        if (out->offsets[t]!=previous || end>bytes) return MH3_PACK_RANGE;
        previous=(uint32_t)end;
    }
    out->data_offset=previous;
    for (i=84;i<128;i+=4) if (le32(metadata.data+i)) return MH3_PACK_UNSUPPORTED;
    for (i=0;i<out->counts[MH3_PACK_LAYOUTS];++i) { mh3_pack_layout x; s=mh3_mesh_pack_layout(out,i,&x); if (s) return s; }
    for (i=0;i<out->counts[MH3_PACK_MODELS];++i) {
        mh3_pack_model x; mh3_pack_recipes recipes; uint32_t n,firsts[4],counts[4];
        const mh3_pack_table tables[4]={MH3_PACK_NODES,MH3_PACK_MESHES,MH3_PACK_MATERIALS,MH3_PACK_DRAWS};
        s=mh3_mesh_pack_model(out,i,&x); if (s) return s;
        for (n=0;n<i;++n) if (le32(record(out,MH3_PACK_MODELS,n))==x.id) return MH3_PACK_INVALID;
        firsts[0]=x.node_first; firsts[1]=x.mesh_first; firsts[2]=x.material_first; firsts[3]=x.draw_first;
        counts[0]=x.node_count; counts[1]=x.mesh_count; counts[2]=x.material_count; counts[3]=x.draw_count;
        for (t=0;t<4;++t) {
            if (firsts[t]!=model_ends[t]) return MH3_PACK_INVALID;
            model_ends[t]+=counts[t];
            for (n=0;n<counts[t];++n) {
                const uint8_t *r=record(out,tables[t],firsts[t]+n);
                if (t<3 && le32(r+(t==0?0:4))!=x.id) return MH3_PACK_INVALID;
                if ((t==1 || t==2) && (le32(r+8)>65534 || le32(r)!=((x.id<<16)|(le32(r+8)+1)))) return MH3_PACK_INVALID;
                if (t==0) {
                    int32_t parent=signed_word(le32(r+8)),child=signed_word(le32(r+24));
                    if ((parent>=0 && ((uint32_t)parent<x.node_first || (uint32_t)parent>=x.node_first+x.node_count)) ||
                        (child>=0 && ((uint32_t)child<x.node_first || (uint32_t)child>=x.node_first+x.node_count))) return MH3_PACK_INVALID;
                }
                if (t==3) {
                    uint32_t mesh=le32(r),mat=le32(r+4),node=le32(r+8);
                    if (mesh<x.mesh_first || mesh>=x.mesh_first+x.mesh_count || mat<x.material_first || mat>=x.material_first+x.material_count ||
                        node<x.node_first || node>=x.node_first+x.node_count || le32(r+24)!=le32(record(out,MH3_PACK_MESHES,mesh))) return MH3_PACK_INVALID;
                }
            }
        }
        s=mh3_mesh_pack_recipes(out,i,&recipes); if (s) return s;
        for (n=0;n<recipes.command_count;++n) {
            mh3_pack_recipe c; s=mh3_mesh_pack_recipe(&recipes,n,&c); if (s) return s;
            if ((c.opcode==2 && (c.destination_or_node>=x.node_count || c.source_or_parent>=recipes.matrix_count)) ||
                (c.opcode!=2 && c.destination_or_node>=recipes.matrix_count) ||
                (c.opcode==5 && c.source_or_parent>=x.node_count) ||
                (c.opcode==6 && c.source_or_parent>=recipes.matrix_count)) return MH3_PACK_RANGE;
        }
        for (n=0;n<recipes.weight_count;++n) { mh3_pack_weight w; s=mh3_mesh_pack_weight(&recipes,n,&w); if (s) return s; }
        for (n=0;n<recipes.matrix_count;++n) {
            int32_t node; s=mh3_mesh_pack_matrix_node(&recipes,n,&node); if (s) return s;
            if (node < -1 || (node>=0 && (uint32_t)node>=x.node_count)) return MH3_PACK_RANGE;
        }
        for (n=0;n<x.node_count;++n) if (le32(record(out,MH3_PACK_NODES,x.node_first+n)+12)>=recipes.matrix_count) return MH3_PACK_RANGE;
        for (n=0;n<x.node_count;++n) {
            int32_t cursor=(int32_t)(x.node_first+n); uint32_t steps=0;
            while (cursor>=0) {
                if (++steps>x.node_count) return MH3_PACK_INVALID;
                cursor=signed_word(le32(record(out,MH3_PACK_NODES,(uint32_t)cursor)+8));
            }
        }
        for (n=0;n<x.mesh_count;++n) if (le32(record(out,MH3_PACK_MESHES,x.mesh_first+n)+44)>=recipes.matrix_count) return MH3_PACK_RANGE;
    }
    if (model_ends[0]!=out->counts[MH3_PACK_NODES] || model_ends[1]!=out->counts[MH3_PACK_MESHES] ||
        model_ends[2]!=out->counts[MH3_PACK_MATERIALS] || model_ends[3]!=out->counts[MH3_PACK_DRAWS]) return MH3_PACK_INVALID;
    for (i=0;i<out->counts[MH3_PACK_NODES];++i) { mh3_pack_node x; s=mh3_mesh_pack_node(out,i,&x); if (s) return s; }
    for (i=0;i<out->counts[MH3_PACK_MATERIALS];++i) {
        mh3_pack_material x; uint32_t n; s=mh3_mesh_pack_material(out,i,&x); if (s) return s;
        if (x.binding_first!=binding_total) return MH3_PACK_INVALID;
        binding_total+=x.binding_count;
        for (n=0;n<x.binding_count;++n) {
            mh3_pack_binding b; s=mh3_mesh_pack_binding(out,x.binding_first+n,&b); if (s) return s;
            if (b.material_index!=i) return MH3_PACK_INVALID;
        }
    }
    if (binding_total!=out->counts[MH3_PACK_BINDINGS]) return MH3_PACK_INVALID;
    for (i=0;i<out->counts[MH3_PACK_DRAWS];++i) { mh3_pack_draw x; s=mh3_mesh_pack_draw(out,i,&x); if (s) return s; }
    for (i=0;i<out->counts[MH3_PACK_MESHES];++i) {
        mh3_pack_mesh mesh; uint32_t n,expected=0;
        s=mh3_mesh_pack_mesh(out,i,&mesh); if (s) return s;
        if (mesh.component_first!=component_total) return MH3_PACK_INVALID;
        component_total+=mesh.component_count;
        for (n=0;n<mesh.component_count;++n) {
            mh3_pack_component c; s=mh3_mesh_pack_component(out,mesh.component_first+n,&c); if (s) return s;
            if (c.mesh_index!=i || c.first_index!=expected || !range(c.first_index,c.index_count,mesh.index_count)) return MH3_PACK_INVALID;
            expected+=c.index_count;
        }
        if (mesh.component_count && expected!=mesh.index_count) return MH3_PACK_INVALID;
    }
    if (component_total!=out->counts[MH3_PACK_COMPONENTS]) return MH3_PACK_INVALID;
    return MH3_PACK_OK;
}

/* Publish a pack only after all tables and typed payloads are validated. */
mh3_pack_status mh3_mesh_pack_open_metadata(mh3_pack_span metadata,mh3_mesh_pack *out) {
    mh3_mesh_pack validated;
    mh3_pack_status status;
    if (!out) return MH3_PACK_INVALID;
    status=open_metadata_validated(metadata,&validated);
    if (status==MH3_PACK_OK) *out=validated;
    return status;
}

mh3_pack_status mh3_mesh_pack_bind_gpu(mh3_mesh_pack *p,mh3_pack_span gpu) {
    if (!p || !p->metadata.data) return MH3_PACK_INVALID;
    if (!gpu.data || gpu.size<p->gpu_bytes) return MH3_PACK_RANGE;
    if ((uintptr_t)gpu.data&15) return MH3_PACK_ALIGNMENT;
    p->gpu=gpu; p->gpu.size=p->gpu_bytes;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_validate_indices(const mh3_mesh_pack *p) {
    uint32_t i;
    if (!p || !p->gpu.data) return MH3_PACK_INVALID;
    for (i=0;i<p->counts[MH3_PACK_MESHES];++i) {
        mh3_pack_mesh mesh; uint32_t n; mh3_pack_status s=mh3_mesh_pack_mesh(p,i,&mesh); if (s) return s;
        for (n=0;n<mesh.index_count;++n) if (le16(mesh.indices.data+(size_t)n*2)>=mesh.vertex_count) return MH3_PACK_RANGE;
        for (n=0;n<mesh.template_indices;++n) if (le16(p->metadata.data+mesh.template_offset+(size_t)n*2)>=mesh.vertex_count) return MH3_PACK_RANGE;
    }
    return MH3_PACK_OK;
}

static int after(uint32_t a,uint32_t b,const float *keys) {
    return keys[a]<keys[b] || (keys[a]==keys[b] && a>b);
}
static void sift(uint32_t *order,uint32_t root,uint32_t count,const float *keys) {
    for (;;) {
        uint32_t child,selected,temp;
        if (root>=count/2) return;
        child=root*2+1; selected=root;
        if (after(order[child],order[selected],keys)) selected=child;
        if (child+1<count && after(order[child+1],order[selected],keys)) selected=child+1;
        if (selected==root) return;
        temp=order[root]; order[root]=order[selected]; order[selected]=temp; root=selected;
    }
}
mh3_pack_status mh3_mesh_pack_sort_components(const mh3_mesh_pack *p,uint32_t i,const float row[4],
                                              uint32_t *order,float *keys,size_t capacity) {
    mh3_pack_mesh mesh; uint32_t n,count; mh3_pack_status s=mh3_mesh_pack_mesh(p,i,&mesh); if (s) return s;
    if (!mesh.component_count) return MH3_PACK_UNSUPPORTED;
    if (!row || !order || !keys || capacity<mesh.component_count) return MH3_PACK_RANGE;
    for (n=0;n<4;++n) if (!isfinite(row[n])) return MH3_PACK_INVALID;
    count=mesh.component_count;
    for (n=0;n<count;++n) {
        mh3_pack_component c; s=mh3_mesh_pack_component(p,mesh.component_first+n,&c); if (s) return s;
        keys[n]=row[0]*c.centroid[0]+row[1]*c.centroid[1]+row[2]*c.centroid[2]+row[3];
        if (!isfinite(keys[n])) return MH3_PACK_INVALID;
        order[n]=n;
    }
    for (n=count/2;n>0;--n) sift(order,n-1,count,keys);
    for (n=count;n>1;--n) { uint32_t temp=order[0]; order[0]=order[n-1]; order[n-1]=temp; sift(order,0,n-1,keys); }
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_write_ordered_indices(const mh3_mesh_pack *p,uint32_t i,const uint32_t *order,size_t count,
                                                   uint8_t *scratch,size_t scratch_bytes,void *destination,size_t destination_bytes) {
    mh3_pack_mesh mesh; size_t n,written=0; uint8_t *dst=destination;
    mh3_pack_status s=mh3_mesh_pack_mesh(p,i,&mesh); if (s) return s;
    if (!mesh.component_count) return MH3_PACK_UNSUPPORTED;
    if (!order || !scratch || !dst || count!=mesh.component_count || scratch_bytes<count || destination_bytes<mesh.index_bytes) return MH3_PACK_RANGE;
    if ((uintptr_t)dst&1) return MH3_PACK_ALIGNMENT;
    memset(scratch,0,count);
    /* Validate before writing, so rejected permutations don't partially modify
     * a GPU-owned frame-ring slot. Ring lifetime is still caller responsibility. */
    for (n=0;n<count;++n) {
        if (order[n]>=count || scratch[order[n]]) return MH3_PACK_INVALID;
        scratch[order[n]]=1;
    }
    for (n=0;n<count;++n) {
        mh3_pack_component c; size_t bytes;
        s=mh3_mesh_pack_component(p,mesh.component_first+order[n],&c); if (s) return s;
        bytes=(size_t)c.index_count*2;
        if (bytes>destination_bytes-written) return MH3_PACK_RANGE;
        memcpy(dst+written,p->metadata.data+mesh.template_offset+(size_t)c.first_index*2,bytes);
        written+=bytes;
    }
    return written==mesh.index_bytes?MH3_PACK_OK:MH3_PACK_INVALID;
}
