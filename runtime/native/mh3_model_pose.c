#include "mh3_model_pose.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static uint32_t canonical(uint32_t address) {
    return address?(address&UINT32_C(0x3fffffff))|UINT32_C(0x80000000):0;
}
static int intersects(uint32_t a,uint32_t size,uint32_t b,uint32_t bytes) {
    return size && bytes && (uint64_t)canonical(a)+size>canonical(b) &&
        (uint64_t)canonical(b)+bytes>canonical(a);
}
mh3_res_status mh3_model_pose_count(mh3_res_span scene,uint32_t *out) {
    if(!out || !scene.data) return MH3_RES_INVALID;
    if(scene.size<0x104) return MH3_RES_RANGE;
    *out=(uint32_t)scene.data[0x102]<<8|scene.data[0x103];return MH3_RES_OK;
}
mh3_res_status mh3_model_pose_owner_read(const mh3_res_memory *memory,uint32_t scene,mh3_model_pose_owner *out) {
    if(!memory || !out || !scene || (scene&3)) return MH3_RES_INVALID;
    mh3_res_memory_view view;mh3_res_status status=mh3_res_runtime_map(memory,scene,0x104,&view);
    if(status) return status;
    const uint8_t *p=view.span.data+view.offset;
    mh3_model_pose_owner result;memset(&result,0,sizeof result);
    result.scene_address=canonical(scene);result.model_address=canonical(word(p+0xe8));
    result.views=p[0x100];result.current_view=p[0x101];
    result.count=(uint32_t)p[0x102]<<8|p[0x103];
    if(!result.views || result.views>16 || result.current_view>=result.views || !result.model_address)
        return MH3_RES_INVALID;
    status=mh3_res_runtime_map(memory,result.model_address,0x70,&view);if(status) return status;
    const uint8_t *mdl=view.span.data+view.offset;
    if(word(mdl)!=UINT32_C(0x4d444c30)) return MH3_RES_INVALID;
    if(word(mdl+8)!=11) return MH3_RES_UNSUPPORTED;
    if(word(mdl+0x68)!=result.count) return MH3_RES_INVALID;
    for(unsigned i=0;i<3;i++) {
        result.base[i]=canonical(word(p+0xf4+i*4));
        result.slice_bytes[i]=(result.count*(i==1?36u:48u)+31u)&~31u;
        if(!result.base[i]) {
            if(!i && result.count) return MH3_RES_INVALID;
            continue;
        }
        /* LC output stores address whole32-byte blocks. An unaligned custom
         * allocation cannot certify the pointer supplied to that producer. */
        if(result.base[i]&31) return MH3_RES_UNSUPPORTED;
        /* Construct8007F148 allocates every view, including aligned padding. */
        status=mh3_res_runtime_map(memory,result.base[i],(size_t)result.slice_bytes[i]*result.views,&view);
        if(status) return status;
    }
    *out=result;return MH3_RES_OK;
}
mh3_res_status mh3_model_pose_array(const mh3_model_pose_owner *owner,uint32_t kind,uint32_t *out) {
    if(!owner || !out || kind>=3 || !owner->views || owner->views>16 || owner->current_view>=owner->views)
        return MH3_RES_INVALID;
    uint32_t value=0;
    if(owner->base[kind]) {
        const uint64_t target=(uint64_t)owner->base[kind]+owner->current_view*(uint64_t)owner->slice_bytes[kind];
        if(target>UINT32_MAX) return MH3_RES_RANGE;
        value=(uint32_t)target;
    }
    *out=value;return MH3_RES_OK;
}
mh3_res_status mh3_model_pose_publish(mh3_model_pose_owner *owner,uint32_t position,uint32_t normal,
    uint32_t environment,uint32_t count,uint32_t model,uint64_t revision) {
    if(!owner || !revision) return MH3_RES_INVALID;
    if(!owner->scene_address || owner->model_address!=canonical(model)) return MH3_RES_ABSENT;
    uint32_t expected[3];
    for(unsigned i=0;i<3;i++) {mh3_res_status s=mh3_model_pose_array(owner,i,&expected[i]);if(s) return s;}
    if(expected[0]!=canonical(position)) return MH3_RES_ABSENT;
    if(count!=owner->count || expected[1]!=canonical(normal) || expected[2]!=canonical(environment)) return MH3_RES_INVALID;
    if(!count) return MH3_RES_ABSENT;
    if(revision<=owner->revision[owner->current_view]) return MH3_RES_INVALID;
    owner->revision[owner->current_view]=revision;return MH3_RES_OK;
}
mh3_res_status mh3_model_pose_sources(const mh3_res_memory *memory,const mh3_model_pose_owner *owner,
    const uint32_t addresses[3],uint32_t model,mh3_matrix_sources *out) {
    if(!memory || !owner || !addresses || !out) return MH3_RES_INVALID;
    if(!owner->scene_address || owner->model_address!=canonical(model)) return MH3_RES_ABSENT;
    if(!owner->count) return MH3_RES_ABSENT;
    for(unsigned v=0;v<owner->views;v++) {
        mh3_model_pose_owner selected=*owner;selected.current_view=(uint8_t)v;uint32_t expected[3];
        for(unsigned i=0;i<3;i++) {mh3_res_status s=mh3_model_pose_array(&selected,i,&expected[i]);if(s) return s;}
        if(expected[0]!=canonical(addresses[0])) continue;
        if(expected[1]!=canonical(addresses[1]) || expected[2]!=canonical(addresses[2])) return MH3_RES_INVALID;
        if(!owner->revision[v]) return MH3_RES_ABSENT;
        mh3_matrix_sources result;memset(&result,0,sizeof result);result.generation=owner->revision[v];
        mh3_matrix_table *tables[3]={&result.positions,&result.normals,&result.environment};
        for(unsigned i=0;i<3;i++) {
            /* CalcView's environment branch is nested inside its normal branch. */
            if(!expected[i] || (i==2 && !expected[1])) continue;
            const size_t stride=i==1?36u:48u;
            mh3_res_memory_view view;mh3_res_status s=mh3_res_runtime_map(memory,expected[i],owner->count*stride,&view);
            if(s) return s;
            *tables[i]=(mh3_matrix_table){{view.span.data+view.offset,owner->count*stride},owner->count,stride,1};
        }
        *out=result;return MH3_RES_OK;
    }
    return MH3_RES_ABSENT;
}
void mh3_model_pose_retire(mh3_model_pose_owner *owner,uint32_t address,uint32_t bytes) {
    if(!owner || !owner->scene_address || !bytes) return;
    if(intersects(owner->scene_address,0x104,address,bytes) || intersects(owner->model_address,0x70,address,bytes)) {
        memset(owner,0,sizeof *owner);return;
    }
    for(unsigned v=0;v<owner->views;v++) for(unsigned i=0;i<3;i++) {
        if(owner->base[i] && intersects(owner->base[i]+v*owner->slice_bytes[i],owner->slice_bytes[i],address,bytes))
            owner->revision[v]=0;
    }
}
mh3_res_status mh3_model_matrix_select(mh3_model_matrix_selectors *state,const uint32_t rows[8]) {
    if(!state || !rows) return MH3_RES_INVALID;
    mh3_model_matrix_selectors result=*state;
    for(unsigned i=0;i<8;i++) {
        if(rows[i]>60 || rows[i]%3) return MH3_RES_UNSUPPORTED;
        result.row[i]=(uint8_t)rows[i];
    }
    result.known=255;*state=result;return MH3_RES_OK;
}
void mh3_model_matrix_reset(mh3_model_matrix_selectors *state) {
    if(!state) return;
    memset(state->row,60,sizeof state->row);state->known=255;
}
mh3_res_status mh3_model_matrix_dual(mh3_model_matrix_selectors *state,int enabled) {
    if(!state || enabled<0 || enabled>1) return MH3_RES_INVALID;
    state->dual_known=1;state->dual_enabled=(uint8_t)enabled;return MH3_RES_OK;
}
mh3_res_status mh3_model_matrix_overlay(const mh3_model_matrix_selectors *selectors,mh3_native_material_state *material) {
    if(!selectors || !material || selectors->dual_known>1 || selectors->dual_enabled>1) return MH3_RES_INVALID;
    for(unsigned i=0;i<8;i++) if((selectors->known&(1u<<i)) && (selectors->row[i]>60 || selectors->row[i]%3))
        return MH3_RES_UNSUPPORTED;
    (void)mh3_model_matrix_overlay_unchecked(selectors,material);
    return MH3_RES_OK;
}
uint16_t mh3_model_matrix_overlay_unchecked(const mh3_model_matrix_selectors *selectors,mh3_native_material_state *material) {
    assert(selectors && material);
    assert(selectors->dual_known<=1 && selectors->dual_enabled<=1);
    uint16_t changed=0;
    for(unsigned i=0;i<8;i++) {
        mh3_pack_texgen *to=&material->texgen[i];
        if(selectors->known&(1u<<i)) {
            assert(selectors->row[i]<=60 && selectors->row[i]%3==0);
            if(!(to->known&(1u<<MH3_TEXGEN_MATRIX_ROW)) || to->values[MH3_TEXGEN_MATRIX_ROW]!=selectors->row[i])
                changed|=(uint16_t)(1u<<i);
            to->known|=1u<<MH3_TEXGEN_MATRIX_ROW;
            to->values[MH3_TEXGEN_MATRIX_ROW]=selectors->row[i];
        }
        if(selectors->dual_known) {
            if(!(to->known&(1u<<MH3_TEXGEN_DUAL_TEXTURE)) || to->values[MH3_TEXGEN_DUAL_TEXTURE]!=selectors->dual_enabled)
                changed|=(uint16_t)(1u<<(8+i));
            to->known|=1u<<MH3_TEXGEN_DUAL_TEXTURE;
            to->values[MH3_TEXGEN_DUAL_TEXTURE]=selectors->dual_enabled;
        }
    }
    return changed;
}
mh3_res_status mh3_model_matrix_rows(const uint32_t sources[8],int32_t matrix_id,uint32_t rows[8],int *apply) {
    if(!sources || !rows || !apply) return MH3_RES_INVALID;
    if(matrix_id<0) {*apply=0;return MH3_RES_OK;}
    uint32_t result[8];
    for(unsigned i=0;i<8;i++) result[i]=sources[i]==1?30+i*3:sources[i]==2?0:60;
    memcpy(rows,result,sizeof result);*apply=1;return MH3_RES_OK;
}
