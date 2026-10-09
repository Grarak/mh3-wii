#include "mh3_model_matrices.h"
#include <math.h>
#include <string.h>
#include <float.h>
_Static_assert(sizeof(float)==4 && FLT_RADIX==2 && FLT_MANT_DIG==24,"Requires IEEE binary32 floats");

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static mh3_res_status read_matrix(mh3_matrix_table t,size_t index,size_t words,void *out) {
    size_t at,i;
    uint8_t *destination=out;
    if (!out || !t.bytes.data) return MH3_RES_INVALID;
    if (index>=t.count || t.stride<words*4 || index>SIZE_MAX/t.stride) return MH3_RES_RANGE;
    at=index*t.stride;
    if (at>t.bytes.size || words*4>t.bytes.size-at) return MH3_RES_RANGE;
    for (i=0;i<words;i++) {
        if (t.big_endian) { uint32_t bits=be32(t.bytes.data+at+i*4); memcpy(destination+i*4,&bits,4); }
        else memcpy(destination+i*4,t.bytes.data+at+i*4,4);
    }
    return MH3_RES_OK;
}
mh3_res_status mh3_matrix_read34(mh3_matrix_table t,size_t i,mh3_matrix34 *out) {
    return out?read_matrix(t,i,12,out):MH3_RES_INVALID;
}
mh3_res_status mh3_matrix_read33(mh3_matrix_table t,size_t i,mh3_matrix33 *out) {
    return out?read_matrix(t,i,9,out):MH3_RES_INVALID;
}
void mh3_matrix_bind(mh3_matrix_binding *state,const uint32_t addresses[3]) {
    if (!state || !addresses) return;
    memcpy(state->address,addresses,sizeof state->address);
    if (++state->generation==0) state->generation=1;
}
static mh3_matrix34 identity(void) {
    mh3_matrix34 m={{{1,0,0,0},{0,1,0,0},{0,0,1,0}}}; return m;
}
static mh3_matrix34 expanded(mh3_matrix33 m) {
    mh3_matrix34 out={{{0}}}; size_t r,c;
    for (r=0;r<3;r++) for (c=0;c<3;c++) out.v[r][c]=m.v[r][c];
    return out;
}
static mh3_matrix33 upper(mh3_matrix34 m) {
    mh3_matrix33 out; size_t r,c;
    for (r=0;r<3;r++) for (c=0;c<3;c++) out.v[r][c]=m.v[r][c];
    return out;
}
void mh3_matrix_concat(const mh3_matrix34 *a,const mh3_matrix34 *b,mh3_matrix34 *out) {
    mh3_matrix34 product; size_t r,c;
    if (!a || !b || !out) return;
    /* Preserve the verified matrix library's multiplication order while using
     * ordinary scalar floats and explicit fused operations. Output may alias. */
    for (r=0;r<3;r++) for (c=0;c<4;c++) {
        float v=a->v[r][0]*b->v[0][c];
        v=fmaf(a->v[r][1],b->v[1][c],v);
        v=fmaf(a->v[r][2],b->v[2][c],v);
        product.v[r][c]=fmaf(c==3?1.0f:0.0f,a->v[r][3],v);
    }
    *out=product;
}
mh3_res_status mh3_matrix_inverse_normal(const mh3_matrix34 *m,mh3_matrix33 *out) {
    mh3_matrix33 n; float det; size_t r,c;
    if (!m || !out) return MH3_RES_INVALID;
    for (r=0;r<3;r++) for (c=0;c<3;c++) {
        size_t a=(r+1)%3,b=(r+2)%3,x=(c+1)%3,y=(c+2)%3;
        n.v[r][c]=fmaf(m->v[a][x],m->v[b][y],-(m->v[a][y]*m->v[b][x]));
    }
    det=fmaf(m->v[0][2],n.v[0][2],fmaf(m->v[0][1],n.v[0][1],m->v[0][0]*n.v[0][0]));
    /* 80330E78 tests |det| against the binary32 constant at 8079D6F0.
     * NaNs are intentionally not classified as singular by this comparison. */
    if (fabsf(det)<1.0e-36f) return MH3_RES_ABSENT;
    for (r=0;r<3;r++) for (c=0;c<3;c++) n.v[r][c]/=det;
    *out=n; return MH3_RES_OK;
}
static mh3_vec3 normalized(mh3_vec3 v) {
    float length_squared=fmaf(v.v[2],v.v[2],v.v[0]*v.v[0])+v.v[1]*v.v[1];
    float reciprocal=1.0f/sqrtf(length_squared); size_t i;
    for (i=0;i<3;i++) v.v[i]*=reciprocal;
    return v;
}
static mh3_res_status command(mh3_matrix_emit emit,void *u,mh3_matrix_action action,
                              uint32_t index,uint32_t channel,const mh3_matrix34 *direct) {
    mh3_matrix_command c; memset(&c,0,sizeof c);
    c.action=action;c.index=index;c.channel=channel;c.direct=direct;
    return emit(u,&c);
}
mh3_res_status mh3_shape_matrix_submit(const mh3_shape_matrix_input *in,
                                      mh3_matrix_emit emit,void *user,
                                      mh3_shape_matrix_result *out) {
    mh3_res_status s; size_t i; int all_identity=1; mh3_matrix_command current;
    if (!in || !out || !emit) return MH3_RES_INVALID;
    memset(out,0,sizeof *out);out->current_is_valid=in->current_is_valid;
    if (!in->valid) return MH3_RES_OK;
    out->envelope=in->matrix_id<0;
    s=command(emit,user,MH3_MATRIX_SCALE_TEXTURES,0,0,NULL);if (s) return s;
    if (out->envelope) {
        if (!out->current_is_valid) {
            s=command(emit,user,MH3_MATRIX_CURRENT_IDENTITY,0,0,NULL);if (s) return s;
            out->current_is_valid=1;
        }
        if (!in->view_array_is_valid) return MH3_RES_INVALID;
    } else {
        uint32_t id=(uint32_t)in->matrix_id&0xffff;
        if (!in->direct_position && !in->view_array_is_valid) return MH3_RES_INVALID;
        s=command(emit,user,in->direct_position?MH3_MATRIX_POSITION_DIRECT:MH3_MATRIX_POSITION_INDEXED,
                  id,0,in->direct_position);if (s) return s;
        if (in->has_normal) {
            const mh3_matrix34 *normal=in->direct_normal?in->direct_normal:in->direct_position;
            s=command(emit,user,in->direct_position?MH3_MATRIX_NORMAL_DIRECT:MH3_MATRIX_NORMAL_INDEXED,
                      id,0,normal);if (s) return s;
        }
        memset(&current,0,sizeof current);current.action=MH3_MATRIX_CURRENT_SELECT;
        for (i=0;i<8;i++) {
            if (in->texture_source[i]==1) {
                current.texture_ids[i]=30+(uint32_t)i*3;all_identity=0;
                s=command(emit,user,MH3_MATRIX_TEXTURE_NORMAL,id,(uint32_t)i,NULL);if (s) return s;
            } else if (in->texture_source[i]==2) { current.texture_ids[i]=0;all_identity=0; }
            else current.texture_ids[i]=60;
        }
        if (!all_identity) {s=emit(user,&current);if (s) return s;out->current_is_valid=0;}
        else if (!out->current_is_valid) {
            s=command(emit,user,MH3_MATRIX_CURRENT_IDENTITY,0,0,NULL);if (s) return s;
            out->current_is_valid=1;
        }
    }
    s=command(emit,user,MH3_MATRIX_GENMODE,0,0,NULL);if (s) return s;
    s=command(emit,user,MH3_MATRIX_PRIMITIVES,0,0,NULL);if (s) return s;
    out->submitted=1;
    if (out->envelope) out->current_is_valid=0;
    return MH3_RES_OK;
}
static mh3_res_status normal_at(const mh3_matrix_sources *s,size_t id,mh3_matrix33 *out) {
    mh3_matrix34 position; mh3_res_status status;
    if (s->normals.bytes.data) return mh3_matrix_read33(s->normals,id,out);
    status=mh3_matrix_read34(s->positions,id,&position);if (status) return status;
    status=mh3_matrix_inverse_normal(&position,out);
    /* MH3's 80087F7C ignores the inverse status. Its inverse routine copies
     * the position's upper 3x3 on a singular input (unlike older ogws' identity
     * fallback). The direct reader owns the result, so no guest cache is needed. */
    if (status==MH3_RES_ABSENT) {*out=upper(position);return MH3_RES_OK;}
    return status;
}
mh3_res_status mh3_model_palette_rigid(const mh3_shape_matrix_input *in,
                                      const mh3_matrix_sources *s,mh3_model_palette *out) {
    size_t i,id; mh3_res_status status;
    if (!in || !s || !out) return MH3_RES_INVALID;
    memset(out,0,sizeof *out);
    for (i=0;i<8;i++) out->texture_ids[i]=60;
    if (!in->valid) return MH3_RES_OK;
    if (in->matrix_id<0) return s->positions.bytes.data?MH3_RES_OK:MH3_RES_INVALID;
    id=(uint32_t)in->matrix_id&0xffff;
    if (in->direct_position) out->position[0]=*in->direct_position;
    else {status=mh3_matrix_read34(s->positions,id,&out->position[0]);if (status) return status;}
    out->position_valid=1;
    if (in->has_normal) {
        if (in->direct_position) out->normal[0]=upper(in->direct_normal?*in->direct_normal:*in->direct_position);
        else {status=mh3_matrix_read33(s->normals,id,&out->normal[0]);if (status) return status;}
        out->normal_valid=1;
    }
    for (i=0;i<8;i++) {
        if (in->texture_source[i]==1) {
            mh3_matrix33 n;status=normal_at(s,id,&n);if (status) return status;
            out->texture[i]=expanded(n);out->texture_valid|=(uint8_t)(1u<<i);out->texture_ids[i]=30+(uint32_t)i*3;
        } else if (in->texture_source[i]==2) out->texture_ids[i]=0;
    }
    return MH3_RES_OK;
}
mh3_res_status mh3_model_palette_load(const mh3_matrix_sources *s,const mh3_matrix_load *load,
                                     mh3_model_palette *p) {
    size_t slot;mh3_res_status status;
    if (!s || !load || !p) return MH3_RES_INVALID;
    if (load->array==0 && load->xf_address<120 && load->xf_address%12==0 && load->word_count==12) {
        slot=load->xf_address/12;status=mh3_matrix_read34(s->positions,load->source_index,&p->position[slot]);
        if (!status) p->position_valid|=(uint16_t)(1u<<slot);
        return status;
    }
    if (load->array==1 && load->xf_address>=0x400 && load->xf_address<0x45a &&
        (load->xf_address-0x400)%9==0 && load->word_count==9) {
        slot=(load->xf_address-0x400)/9;status=mh3_matrix_read33(s->normals,load->source_index,&p->normal[slot]);
        if (!status) p->normal_valid|=(uint16_t)(1u<<slot);
        return status;
    }
    if (load->array==2 && load->xf_address>=120 && load->xf_address<240 &&
        (load->xf_address-120)%12==0 && load->word_count==12) {
        slot=(load->xf_address-120)/12;status=mh3_matrix_read34(s->environment,load->source_index,&p->texture[slot]);
        if (!status) p->texture_valid|=(uint16_t)(1u<<slot);
        return status;
    }
    /* Partial, overlapping or other XF destinations require an explicit
     * converter descriptor extension; do not silently reinterpret them. */
    return MH3_RES_UNSUPPORTED;
}
mh3_res_status mh3_model_indirect_prepare(const mh3_indirect_input *in,mh3_indirect_output *out) {
    size_t i,r,c;int active=0;mh3_matrix34 normal;
    if (!in || !out) return MH3_RES_INVALID;
    memset(out,0,sizeof *out);
    if (!in->material_valid || !in->shape_valid) return MH3_RES_OK;
    for (i=0;i<3;i++) if (in->slot[i].method) active=1;
    if (!active) return MH3_RES_OK;
    out->active=1;
    if (in->shape_matrix_id<0) normal=in->camera;
    else {
        normal=expanded(in->view_normal);
        if (in->shape_matrix_id!=in->node_matrix_id) {
            mh3_matrix34 relative;
            mh3_matrix_concat(&in->shape_node_inverse,&in->node_model,&relative);
            mh3_matrix_concat(&normal,&relative,&normal);
        }
        for (r=0;r<3;r++) normal.v[r][3]=0;
        for (c=0;c<3;c++) {
            mh3_vec3 col={{normal.v[0][c],normal.v[1][c],normal.v[2][c]}};col=normalized(col);
            for (r=0;r<3;r++) normal.v[r][c]=col.v[r];
        }
    }
    out->normal=normal;
    for (i=0;i<3;i++) {
        const mh3_indirect_slot *slot=&in->slot[i];mh3_vec3 light;
        out->matrix[i]=identity();if (!slot->method) continue;
        out->valid_mask|=(uint8_t)(1u<<i);
        if (!slot->light_present || !slot->light_enabled) {memset(&out->matrix[i],0,sizeof out->matrix[i]);continue;}
        if (slot->light_spot) {
            light=slot->light_direction;
            if (light.v[0]==0 && light.v[1]==0 && light.v[2]==0) {
                for (c=0;c<3;c++) light.v[c]=-slot->light_position.v[c];
                light=normalized(light);
            }
        } else if (!slot->light_specular) {
            for (c=0;c<3;c++) light.v[c]=-slot->light_position.v[c];
            light=normalized(light);
        } else {
            float scale=-2.0f*slot->light_direction.v[2];
            for (c=0;c<3;c++) light.v[c]=scale*slot->light_direction.v[c];
            light.v[2]+=1;light=normalized(light);
        }
        out->light[i]=light;out->light_mask|=(uint8_t)(1u<<i);
        for (c=0;c<3;c++) out->matrix[i].v[0][c]=-0.5f*light.v[c];
        if (slot->method==2) {
            mh3_vec3 half=light;half.v[2]-=1;half=normalized(half);
            for (c=0;c<3;c++) out->matrix[i].v[1][c]=-0.5f*half.v[c];
        } else for (c=0;c<3;c++) out->matrix[i].v[1][c]=0;
        mh3_matrix_concat(&out->matrix[i],&normal,&out->matrix[i]);
    }
    return MH3_RES_OK;
}

static mh3_res_status raw(const mh3_res_memory *m,uint32_t address,size_t size,mh3_res_span *out) {
    mh3_res_memory_view v;mh3_res_status s;
    if (!out) return MH3_RES_INVALID;
    s=mh3_res_runtime_map(m,address,size,&v);if (s) return s;
    out->data=v.span.data+v.offset;out->size=size;return MH3_RES_OK;
}
static mh3_res_status reference(const mh3_res_memory *m,uint32_t ref,uint32_t *out) {
    return mh3_res_runtime_reference(m,ref,0,out);
}
static mh3_res_status model_at(const mh3_res_memory *m,uint32_t address,mh3_res_model *out) {
    mh3_res_memory_view v;mh3_res_status s=mh3_res_runtime_map(m,address,16,&v);
    return s?s:mh3_res_model_open(v.span,v.offset,out);
}
static mh3_res_status model_capacity(const mh3_res_model *mdl,uint32_t *count) {
    size_t info;
    if (!mdl || !count) return MH3_RES_INVALID;
    info=mdl->version==11?0x4c:0x40;
    if (mdl->size<info+0x28) return MH3_RES_RANGE;
    *count=be32(mdl->container.data+mdl->offset+info+0x1c);return MH3_RES_OK;
}
static mh3_res_status sources_at(const mh3_res_memory *m,uint32_t state,uint32_t count,mh3_matrix_sources *out) {
    size_t i;mh3_res_span p;mh3_res_status s;mh3_matrix_table *tables[3];
    if (!out) return MH3_RES_INVALID;
    memset(out,0,sizeof *out);tables[0]=&out->positions;tables[1]=&out->normals;tables[2]=&out->environment;
    s=raw(m,state,12,&p);if (s) return s;
    for (i=0;i<3;i++) {
        uint32_t address=be32(p.data+i*4);size_t stride=i==1?36:48;
        if (!address) continue;
        if (count>SIZE_MAX/stride) return MH3_RES_RANGE;
        s=raw(m,address,(size_t)count*stride,&tables[i]->bytes);if (s) return s;
        tables[i]->count=count;tables[i]->stride=stride;tables[i]->big_endian=1;
    }
    return MH3_RES_OK;
}
static mh3_res_status direct_at(const mh3_res_memory *m,uint32_t address,mh3_matrix34 *out) {
    mh3_matrix_table t;mh3_res_status s;memset(&t,0,sizeof t);
    s=raw(m,address,48,&t.bytes);if (s) return s;
    t.count=1;t.stride=48;t.big_endian=1;return mh3_matrix_read34(t,0,out);
}
mh3_res_status mh3_matrix_runtime_shape(const mh3_res_memory *m,const mh3_matrix_runtime_locations *loc,
                                       uint32_t shape_ref,uint32_t direct_position,uint32_t direct_normal,
                                       mh3_matrix_runtime_storage *storage,mh3_shape_matrix_input *in,mh3_matrix_sources *sources) {
    mh3_res_span shape,types,current;mh3_res_model mdl;uint32_t address,parent,count,id;size_t i;mh3_res_status s;
    if (!m || !loc || !storage || !in || !sources) return MH3_RES_INVALID;
    memset(in,0,sizeof *in);memset(storage,0,sizeof *storage);memset(sources,0,sizeof *sources);
    s=reference(m,shape_ref,&address);if (s || !address) return s;
    s=raw(m,address,0x34,&shape);if (s) return s;
    id=be32(shape.data+8);in->matrix_id=id&0x80000000?(int32_t)((int64_t)id-INT64_C(4294967296)):(int32_t)id;
    in->has_normal=(be32(shape.data+0x30)&(1u<<10))!=0;in->valid=1;
    s=mh3_res_runtime_shape_parent(m,shape_ref,&parent);if (s || !parent) return s?s:MH3_RES_INVALID;
    s=model_at(m,parent,&mdl);if (s) return s;
    s=model_capacity(&mdl,&count);if (s) return s;
    s=sources_at(m,loc->arrays,count,sources);if (s) return s;
    in->view_array_is_valid=sources->positions.bytes.data!=NULL;
    s=raw(m,loc->texture_sources,32,&types);if (s) return s;
    s=raw(m,loc->current_valid,1,&current);if (s) return s;
    in->current_is_valid=current.data[0]!=0;
    for (i=0;i<8;i++) in->texture_source[i]=be32(types.data+i*4);
    if (direct_position) {
        s=direct_at(m,direct_position,&storage->direct_position);if (s) return s;
        in->direct_position=&storage->direct_position;
        if (direct_normal) {
            s=direct_at(m,direct_normal,&storage->direct_normal);if (s) return s;
            in->direct_normal=&storage->direct_normal;
        }
    }
    return MH3_RES_OK;
}
static mh3_res_status node_from_matrix(const mh3_res_model *mdl,uint32_t id,size_t *node) {
    size_t info=mdl->offset+(mdl->version==11?0x4c:0x40),table;uint32_t count,value;mh3_res_status s;
    s=mh3_res_relative(mdl->container,info,info+0x24,4,&table);if (s) return s;
    count=be32(mdl->container.data+table);
    if (id>=count || table<mdl->offset || table>mdl->offset+mdl->size ||
        (uint64_t)count*4+4>mdl->offset+mdl->size-table) return MH3_RES_RANGE;
    value=be32(mdl->container.data+table+4+(size_t)id*4);
    if (value>INT32_MAX) return MH3_RES_RANGE;
    return mh3_res_model_data(mdl,MH3_RES_NODES,(int32_t)value,node);
}
static mh3_res_status light_at(const mh3_res_memory *m,uint32_t state,int32_t reference_id,mh3_indirect_slot *out) {
    mh3_res_memory_view v;mh3_res_span light;mh3_matrix_table t;mh3_res_status s;uint32_t set,flags;int64_t map;int32_t id;size_t i;
    s=mh3_res_runtime_map(m,state,20,&v);if (s) return s;
    set=be32(v.span.data+v.offset+16);
    if (set>=128) return MH3_RES_RANGE;
    map=(int64_t)v.offset+9260+(uint64_t)set*12+reference_id;
    if (map<0 || (uint64_t)map>=v.span.size) return MH3_RES_RANGE;
    id=v.span.data[(size_t)map];if (id>=128) return MH3_RES_OK;
    s=raw(m,state+44+(uint32_t)id*68,68,&light);if (s) return s;
    flags=be32(light.data);out->light_present=1;out->light_enabled=(flags&4)!=0;
    out->light_spot=(flags&1)!=0;out->light_specular=(flags&2)!=0;
    memset(&t,0,sizeof t);t.count=1;t.stride=12;t.big_endian=1;
    t.bytes.data=light.data+44;t.bytes.size=12;s=read_matrix(t,0,3,out->light_position.v);if (s) return s;
    t.bytes.data=light.data+56;s=read_matrix(t,0,3,out->light_direction.v);if (s) return s;
    /* Serialized GX light direction stores the negated user direction. This
     * replaces GXGetLightDir's data reads without calling a GX entry. */
    for (i=0;i<3;i++) out->light_direction.v[i]=-out->light_direction.v[i];
    return MH3_RES_OK;
}
mh3_res_status mh3_matrix_runtime_indirect(const mh3_res_memory *m,const mh3_matrix_runtime_locations *loc,
                                          uint32_t material_ref,uint32_t node_ref,uint32_t shape_ref,mh3_indirect_output *out) {
    mh3_indirect_input in;mh3_res_span material,shape,node,camera;mh3_res_model mdl;mh3_matrix_sources sources;
    uint32_t mat,shp,parent,count,node_address,id;size_t i,n;mh3_res_status s;int active=0;
    if (!m || !loc || !out) return MH3_RES_INVALID;
    memset(&in,0,sizeof in);memset(out,0,sizeof *out);
    s=reference(m,material_ref,&mat);if (s || !mat) return s;
    s=reference(m,shape_ref,&shp);if (s || !shp) return s;
    in.material_valid=in.shape_valid=1;
    s=raw(m,mat,0x28,&material);if (s) return s;
    for (i=0;i<3;i++) {in.slot[i].method=material.data[0x20+i];if (in.slot[i].method) active=1;}
    if (!active) return mh3_model_indirect_prepare(&in,out);
    s=raw(m,shp,12,&shape);if (s) return s;
    id=be32(shape.data+8);in.shape_matrix_id=id&0x80000000?(int32_t)((int64_t)id-INT64_C(4294967296)):(int32_t)id;
    if (in.shape_matrix_id<0) {
        uint16_t selected;s=raw(m,loc->camera,4,&camera);if (s) return s;
        selected=(uint16_t)((uint16_t)camera.data[2]<<8|camera.data[3]);
        if (selected>=32) return MH3_RES_RANGE;
        s=direct_at(m,loc->camera+52+(uint32_t)selected*48,&in.camera);if (s) return s;
    } else {
        s=mh3_res_runtime_shape_parent(m,shape_ref,&parent);if (s) return s;
        s=model_at(m,parent,&mdl);if (s) return s;
        s=model_capacity(&mdl,&count);if (s) return s;
        s=sources_at(m,loc->arrays,count,&sources);if (s) return s;
        s=normal_at(&sources,(uint32_t)in.shape_matrix_id,&in.view_normal);if (s) return s;
        s=reference(m,node_ref,&node_address);if (s || !node_address) return s?s:MH3_RES_INVALID;
        s=raw(m,node_address,0xa0,&node);if (s) return s;
        id=be32(node.data+0x10);if (id>INT32_MAX) return MH3_RES_RANGE;in.node_matrix_id=(int32_t)id;
        if (in.shape_matrix_id!=in.node_matrix_id) {
            /* Material parent is intentional: it is what 800940F8 selects. */
            mh3_res_memory_view view;size_t at;mh3_matrix_table table;
            s=mh3_res_runtime_map(m,mat,8,&view);if (s) return s;
            s=mh3_res_relative(view.span,view.offset,view.offset+4,16,&at);if (s) return s;
            s=mh3_res_model_open(view.span,at,&mdl);if (s) return s;
            s=node_from_matrix(&mdl,(uint32_t)in.shape_matrix_id,&n);if (s) return s;
            table.bytes.data=node.data+0x70;table.bytes.size=48;table.count=1;table.stride=48;table.big_endian=1;
            s=mh3_matrix_read34(table,0,&in.node_model);if (s) return s;
            if (n>mdl.offset+mdl.size || 0xd0>mdl.offset+mdl.size-n) return MH3_RES_RANGE;
            table.bytes.data=mdl.container.data+n+0xa0;
            s=mh3_matrix_read34(table,0,&in.shape_node_inverse);if (s) return s;
        }
    }
    for (i=0;i<3;i++) if (in.slot[i].method) {
        int32_t ref=material.data[0x24+i];if (ref>=128) ref-=256;
        s=light_at(m,loc->lights,ref,&in.slot[i]);if (s) return s;
    }
    return mh3_model_indirect_prepare(&in,out);
}
static void put32(uint8_t *p,uint32_t value) {
    p[0]=(uint8_t)(value>>24);p[1]=(uint8_t)(value>>16);p[2]=(uint8_t)(value>>8);p[3]=(uint8_t)value;
}
mh3_res_status mh3_matrix_store_indirect(mh3_res_mut_span object,const mh3_indirect_output *out) {
    size_t i,r,c;
    if (!out || !object.data) return MH3_RES_INVALID;
    if (!out->active) return MH3_RES_OK;
    if (object.size<152) return MH3_RES_RANGE;
    for (i=0;i<3;i++) {
        object.data[4+i]=(uint8_t)((out->valid_mask>>i)&1);
        for (r=0;r<3;r++) for (c=0;c<4;c++) {
            uint32_t bits;memcpy(&bits,&out->matrix[i].v[r][c],4);put32(object.data+8+i*48+(r*4+c)*4,bits);
        }
    }
    return MH3_RES_OK;
}
mh3_res_status mh3_matrix_store_sources(mh3_res_mut_span state,const uint32_t addresses[3]) {
    size_t i;if (!state.data || !addresses) return MH3_RES_INVALID;
    if (state.size<12) return MH3_RES_RANGE;
    for (i=0;i<3;i++) put32(state.data+i*4,addresses[i]);
    return MH3_RES_OK;
}
mh3_res_status mh3_matrix_runtime_operator(const mh3_res_memory *m,uint32_t object,
                                          uint32_t reset_entry,uint32_t set_entry) {
    mh3_res_span header,table;mh3_res_status s;
    s=raw(m,object,4,&header);if (s) return s;
    s=raw(m,be32(header.data),24,&table);if (s) return s;
    return be32(table.data+16)==reset_entry && be32(table.data+20)==set_entry?
           MH3_RES_OK:MH3_RES_UNSUPPORTED;
}
