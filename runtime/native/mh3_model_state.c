/* RMHP08 Camera/Fog/LightState resource helpers, ordinary C11.
 * Guest routines: 800752bc..8007592c,8007a30c/3b8,8008551c/85758.
 * Matrix conventions and resource offsets are validated against the local
 * decompilation. No SDK GX calls or source command-list decoding. */
#include "mh3_model_state.h"
#include <math.h>
#include <string.h>
static uint32_t be32(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint16_t be16(const uint8_t *p) {return (uint16_t)((uint32_t)p[0]<<8|p[1]);}
static float real(const uint8_t *p) {uint32_t b=be32(p);float f;memcpy(&f,&b,4);return f;}
static void put32(uint8_t *p,uint32_t x) {p[0]=(uint8_t)(x>>24);p[1]=(uint8_t)(x>>16);p[2]=(uint8_t)(x>>8);p[3]=(uint8_t)x;}
static void put16(uint8_t *p,uint16_t x) {p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static void putfloat(uint8_t *p,float f) {uint32_t b;memcpy(&b,&f,4);put32(p,b);}
static int finite_values(const float *p,size_t n) {size_t i;for (i=0;i<n;i++) if (!isfinite(p[i])) return 0;return 1;}
void mh3_texture_source_created(mh3_native_texture_source *s,uint32_t resource,uint32_t payload,uint64_t generation,int mutable_source) {
    if (!s) return;
    s->resource_address=resource;s->payload_address=payload;s->generation=generation;s->mutable_source=(uint8_t)(mutable_source!=0);
}
void mh3_texture_source_updated(mh3_native_texture_source *s,uint64_t generation) {if (s && generation>s->generation) s->generation=generation;}
mh3_res_status mh3_texture_source_read(mh3_res_span bytes,uint32_t address,mh3_native_texture_source *out) {
    mh3_native_texture_source x;uint32_t size,offset,tag;
    if (!bytes.data || !out) return MH3_RES_INVALID;
    if (bytes.size<64) return MH3_RES_RANGE;
    if (memcmp(bytes.data,"TEX0",4)) return MH3_RES_INVALID;
    if (be32(bytes.data+8)!=1 && be32(bytes.data+8)!=3) return MH3_RES_UNSUPPORTED;
    size=be32(bytes.data+4);offset=be32(bytes.data+16);
    if (size>bytes.size || size<64 || offset<64 || offset>=size || (uint64_t)address+offset>UINT32_MAX) return MH3_RES_RANGE;
    memset(&x,0,sizeof x);x.resource_address=address;x.payload_address=address+offset;
    x.indexed=(uint8_t)(be32(bytes.data+24)&1);x.width=be16(bytes.data+28);x.height=be16(bytes.data+30);
    x.format=be32(bytes.data+32);x.levels=be32(bytes.data+36);x.min_lod=real(bytes.data+40);x.max_lod=real(bytes.data+44);
    if (!x.width || !x.height || !x.levels || !isfinite(x.min_lod) || !isfinite(x.max_lod) || x.min_lod>x.max_lod) return MH3_RES_INVALID;
    if (x.format>14 || x.format==7 || (x.format>10 && x.format<14)) return MH3_RES_UNSUPPORTED;
    tag=be32(bytes.data+60);if ((tag&0xf0000000u)==0xa0000000u) x.asset_id=tag&0x0fffffffu;
    *out=x;return MH3_RES_OK;
}
mh3_res_status mh3_palette_source_read(mh3_res_span bytes,uint32_t address,mh3_native_palette_source *out) {
    mh3_native_palette_source x;uint32_t size,offset;
    if (!bytes.data || !out) return MH3_RES_INVALID;
    if (bytes.size<32) return MH3_RES_RANGE;
    if (memcmp(bytes.data,"PLT0",4)) return MH3_RES_INVALID;
    if (be32(bytes.data+8)!=1 && be32(bytes.data+8)!=3) return MH3_RES_UNSUPPORTED;
    size=be32(bytes.data+4);offset=be32(bytes.data+16);
    memset(&x,0,sizeof x);x.resource_address=address;x.format=be32(bytes.data+24);x.entries=be16(bytes.data+28);
    if (size>bytes.size || offset<32 || offset>size || (uint64_t)x.entries*2>size-offset || (uint64_t)address+offset>UINT32_MAX) return MH3_RES_RANGE;
    if (x.format>2 || !x.entries) return MH3_RES_INVALID;
    x.payload_address=address+offset;*out=x;return MH3_RES_OK;
}
mh3_res_status mh3_texture_binding_read(mh3_res_span record,const mh3_native_texture_source *source,mh3_native_texture_binding *out) {
    mh3_native_texture_binding b;const uint8_t *p=record.data;
    if (!p || !source || !out) return MH3_RES_INVALID;
    if (record.size<52) return MH3_RES_RANGE;
    memset(&b,0,sizeof b);b.source=*source;b.map_id=be32(p+16);b.tlut_id=be32(p+20);
    b.sampler.wrap_s=be32(p+24);b.sampler.wrap_t=be32(p+28);b.sampler.min_filter=be32(p+32);
    b.sampler.mag_filter=be32(p+36);b.sampler.lod_bias=real(p+40);b.sampler.anisotropy=be32(p+44);
    b.sampler.bias_clamp=p[48];b.sampler.edge_lod=p[49];
    if (b.map_id>=8 || b.sampler.wrap_s>2 || b.sampler.wrap_t>2 || b.sampler.min_filter>5 ||
        b.sampler.mag_filter>1 || b.sampler.anisotropy>2 || !isfinite(b.sampler.lod_bias) ||
        b.sampler.bias_clamp>1 || b.sampler.edge_lod>1) return MH3_RES_INVALID;
    *out=b;return MH3_RES_OK;
}
mh3_res_status mh3_texture_info_store_source(mh3_res_mut_span record,uint32_t resource,int palette) {
    if (!record.data) return MH3_RES_INVALID;
    if (record.size<16) return MH3_RES_RANGE;
    put32(record.data+(palette?12:8),resource);return MH3_RES_OK;
}
mh3_res_status mh3_resource_objects_present(mh3_res_span record,uint32_t slot,int palette,int *out) {
    if (!record.data || !out) return MH3_RES_INVALID;
    if (slot>=8 || record.size<4u+8u*(palette?12u:32u)) return MH3_RES_RANGE;
    *out=(be32(record.data)&(1u<<slot))!=0;return MH3_RES_OK;
}
mh3_res_status mh3_resource_objects_validate(mh3_res_mut_span record,uint32_t slot,int palette) {
    if (!record.data) return MH3_RES_INVALID;
    if (slot>=8 || record.size<4u+8u*(palette?12u:32u)) return MH3_RES_RANGE;
    put32(record.data,be32(record.data)|(1u<<slot));return MH3_RES_OK;
}
mh3_res_status mh3_resource_objects_copy(mh3_res_mut_span destination,mh3_res_span source,int palette) {
    uint8_t bytes[260];uint32_t present;size_t size=palette?100:260,i;
    if (!destination.data || !source.data) return MH3_RES_INVALID;
    if (destination.size<size || source.size<size) return MH3_RES_RANGE;
    present=be32(source.data);if (present&~255u) return MH3_RES_INVALID;
    memcpy(bytes,destination.data,size);put32(bytes,present);
    if (palette) {
        size_t copied=present==0?0:!(present&252u)?32:!(present&224u)?64:96;
        if (copied) memcpy(bytes+4,source.data+4,copied);
    } else for (i=0;i<8;i++) if (present&(1u<<i)) memcpy(bytes+4+i*32,source.data+4+i*32,32);
    memcpy(destination.data,bytes,size);return MH3_RES_OK;
}
mh3_res_status mh3_resource_objects_cache(mh3_res_mut_span state,mh3_res_span objects,int palette,uint8_t *changed) {
    size_t stride=palette?12:32,bytes=8*stride,i;uint32_t present,valid;uint8_t mask=0;
    if (!state.data || !objects.data || !changed) return MH3_RES_INVALID;
    if (state.size<bytes+(palette?2:1) || objects.size<bytes+4) return MH3_RES_RANGE;
    present=be32(objects.data);valid=palette?be16(state.data+bytes):state.data[bytes];
    for (i=0;i<8;i++) if ((present&(1u<<i)) && (!(valid&(1u<<i)) ||
        memcmp(state.data+i*stride,objects.data+4+i*stride,stride))) mask|=(uint8_t)(1u<<i);
    for (i=0;i<8;i++) if (mask&(1u<<i)) memmove(state.data+i*stride,objects.data+4+i*stride,stride);
    valid|=mask;if (palette) put16(state.data+bytes,(uint16_t)valid);else state.data[bytes]=(uint8_t)valid;
    *changed=mask;return MH3_RES_OK;
}
mh3_res_status mh3_texture_scale_size(mh3_res_mut_span state,uint32_t slot,uint16_t width,uint16_t height) {
    if (!state.data) return MH3_RES_INVALID;
    if (slot>=8 || state.size<36) return MH3_RES_RANGE;
    put16(state.data+4+slot*4,width);put16(state.data+6+slot*4,height);
    put32(state.data,(be32(state.data)|2)&~1u);return MH3_RES_OK;
}
mh3_res_status mh3_texture_cache_palette_changed(mh3_res_mut_span state,uint8_t changed) {
    if (!state.data) return MH3_RES_INVALID;
    if (state.size<257) return MH3_RES_RANGE;
    state.data[256]&=(uint8_t)~changed;return MH3_RES_OK;
}
mh3_res_status mh3_depth_compare_cache(mh3_res_mut_span state,mh3_res_mut_span sync,int before,int *apply) {
    uint32_t flags;uint8_t value=(uint8_t)(before!=0);
    if (!state.data || !sync.data || !apply) return MH3_RES_INVALID;
    if (state.size<5 || sync.size<1) return MH3_RES_RANGE;
    flags=be32(state.data);
    *apply=!(flags&1) || state.data[4]!=value;
    if (*apply) {put32(state.data,flags|1);state.data[4]=value;sync.data[0]=1;}
    return MH3_RES_OK;
}
mh3_res_status mh3_fog_read(mh3_res_span record,mh3_native_fog *out) {
    mh3_native_fog x;size_t i;
    if (!record.data || !out) return MH3_RES_INVALID;
    if (record.size<48) return MH3_RES_RANGE;
    memset(&x,0,sizeof x);x.type=be32(record.data);x.start=real(record.data+4);x.end=real(record.data+8);
    x.near_plane=real(record.data+12);x.far_plane=real(record.data+16);memcpy(&x.color,record.data+20,4);
    x.range_enabled=record.data[24];x.range_center=be16(record.data+26);
    for (i=0;i<10;i++) x.range_adjust[i]=be16(record.data+28+i*2);
    if (x.range_enabled>1 || !isfinite(x.start) || !isfinite(x.end) || !isfinite(x.near_plane) || !isfinite(x.far_plane)) return MH3_RES_INVALID;
    *out=x;return MH3_RES_OK;
}
mh3_res_status mh3_fog_range_store(mh3_res_mut_span record,uint16_t center,const uint16_t values[10]) {
    size_t i;if (!record.data || !values) return MH3_RES_INVALID;
    if (record.size<48) return MH3_RES_RANGE;
    put16(record.data+26,center);for (i=0;i<10;i++) put16(record.data+28+i*2,values[i]);return MH3_RES_OK;
}
mh3_res_status mh3_fog_state_select(mh3_res_mut_span state,uint32_t id,mh3_native_fog *out,int *apply) {
    mh3_native_fog fog;mh3_res_status status;uint32_t flags;
    if (!state.data || !out || !apply) return MH3_RES_INVALID;
    if (state.size<8) return MH3_RES_RANGE;
    flags=be32(state.data);
    if ((flags&3)==3 && be32(state.data+4)==id) {*apply=0;return MH3_RES_OK;}
    memset(&fog,0,sizeof fog);
    if (id<32) {
        if (state.size<8+(size_t)(id+1)*48) return MH3_RES_RANGE;
        status=mh3_fog_read((mh3_res_span){state.data+8+(size_t)id*48,48},&fog);
        if (status) return status;
    }
    put32(state.data,flags|3);put32(state.data+4,id);*out=fog;*apply=1;return MH3_RES_OK;
}
mh3_res_status mh3_camera_read(mh3_res_span record,mh3_native_camera *out) {
    mh3_native_camera x;size_t i;const uint8_t *p=record.data;
    if (!p || !out) return MH3_RES_INVALID;
    if (record.size<268) return MH3_RES_RANGE;
    memset(&x,0,sizeof x);
    for (i=0;i<12;i++) x.view.v[i/4][i%4]=real(p+i*4);
    for (i=0;i<16;i++) x.projection[i]=real(p+48+i*4);
    x.flags=be32(p+112);x.projection_type=be32(p+168);
    for (i=0;i<3;i++) {x.position.v[i]=real(p+116+i*4);x.up.v[i]=real(p+128+i*4);x.target.v[i]=real(p+140+i*4);x.rotation.v[i]=real(p+152+i*4);}
    x.twist=real(p+164);x.fovy=real(p+172);x.aspect=real(p+176);x.near_plane=real(p+180);x.far_plane=real(p+184);
    x.top=real(p+188);x.bottom=real(p+192);x.left=real(p+196);x.right=real(p+200);
    for (i=0;i<6;i++) {x.viewport[i]=real(p+220+i*4);if (!isfinite(x.viewport[i])) return MH3_RES_INVALID;}
    for (i=0;i<4;i++) x.scissor[i]=be32(p+244+i*4);
    x.scissor_offset[0]=be32(p+260);x.scissor_offset[1]=be32(p+264);
    if (x.projection_type>1) return MH3_RES_UNSUPPORTED;
    *out=x;return MH3_RES_OK;
}
mh3_res_status mh3_camera_prepare_projection(mh3_native_camera *c) {
    float m[16]={0},inv,cot,horizontal,vertical;
    if (!c) return MH3_RES_INVALID;
    if (c->flags&128) return finite_values(c->projection,16)?MH3_RES_OK:MH3_RES_INVALID;
    if (!isfinite(c->near_plane) || !isfinite(c->far_plane) || c->near_plane==c->far_plane) return MH3_RES_INVALID;
    inv=1.0f/(c->far_plane-c->near_plane);
    m[10]=-c->near_plane*inv;m[11]=inv*-(c->far_plane*c->near_plane);m[14]=-1.0f;
    if (c->flags&(64|16)) {
        if (c->left==c->right || c->top==c->bottom) return MH3_RES_INVALID;
        horizontal=1.0f/(c->right-c->left);vertical=1.0f/(c->top-c->bottom);
        if (c->flags&64) {
            m[0]=2.0f*horizontal;m[5]=2.0f*vertical;
            m[3]=horizontal*-(c->right+c->left);m[7]=vertical*-(c->top+c->bottom);
            m[10]=-inv;m[11]=-c->far_plane*inv;m[14]=0.0f;m[15]=1.0f;
        } else {
            m[0]=(2.0f*c->near_plane)*horizontal;m[5]=(2.0f*c->near_plane)*vertical;
            m[2]=horizontal*(c->right+c->left);m[6]=vertical*(c->top+c->bottom);
        }
    } else {
        if (!(c->fovy>0.0f && c->fovy<180.0f) || c->aspect==0.0f) return MH3_RES_INVALID;
        cot=1.0f/tanf((3.1415927410125732f/180.0f)*(0.5f*c->fovy));m[0]=cot/c->aspect;m[5]=cot;
    }
    if (!finite_values(m,16)) return MH3_RES_INVALID;
    memcpy(c->projection,m,sizeof m);c->flags|=128;return MH3_RES_OK;
}
static float dot(const mh3_vec3 *a,const mh3_vec3 *b) {return (a->v[0]*b->v[0]+a->v[1]*b->v[1])+a->v[2]*b->v[2];}
static mh3_vec3 cross(mh3_vec3 a,mh3_vec3 b) {return (mh3_vec3){{a.v[1]*b.v[2]-a.v[2]*b.v[1],a.v[2]*b.v[0]-a.v[0]*b.v[2],a.v[0]*b.v[1]-a.v[1]*b.v[0]}};}
static int normalize(mh3_vec3 *v) {float squared=dot(v,v),r;size_t i;if (!(squared>0.0f) || !isfinite(squared)) return 0;r=1.0f/sqrtf(squared);for (i=0;i<3;i++) v->v[i]*=r;return 1;}
mh3_res_status mh3_camera_prepare_view(mh3_native_camera *c) {
    mh3_vec3 back,right,up;mh3_matrix34 view;size_t i;float s,t,radians=3.1415927410125732f/180.0f;
    if (!c) return MH3_RES_INVALID;
    if (c->flags&8) return finite_values(&c->view.v[0][0],12)?MH3_RES_OK:MH3_RES_INVALID;
    if (c->flags&(1|4)) {
        for (i=0;i<3;i++) back.v[i]=c->position.v[i]-c->target.v[i];
        if (!(c->flags&1) && back.v[0]==0.0f && back.v[2]==0.0f) {
            right=(mh3_vec3){{1,0,0}};up=(mh3_vec3){{0,0,back.v[1]<=0?1.0f:-1.0f}};back=(mh3_vec3){{0,back.v[1]<=0?-1.0f:1.0f,0}};
        } else {
            if (!normalize(&back)) return MH3_RES_INVALID;
            right=cross(c->flags&1?c->up:(mh3_vec3){{0,1,0}},back);
            if (!normalize(&right)) return MH3_RES_INVALID;
            up=cross(back,right);
            if (!(c->flags&1)) {
                mh3_vec3 old=right;s=sinf(c->twist*radians);t=cosf(c->twist*radians);
                for (i=0;i<3;i++) {right.v[i]=s*up.v[i]+t*old.v[i];up.v[i]=t*up.v[i]-s*old.v[i];}
            }
        }
    } else {
        float sx=sinf(c->rotation.v[0]*radians),cx=cosf(c->rotation.v[0]*radians),sy=sinf(c->rotation.v[1]*radians),cy=cosf(c->rotation.v[1]*radians),sz=sinf(c->rotation.v[2]*radians),cz=cosf(c->rotation.v[2]*radians);
        right=(mh3_vec3){{sx*sy*sz+cy*cz,cx*sz,sx*cy*sz-sy*cz}};
        up=(mh3_vec3){{sx*sy*cz-cy*sz,cx*cz,sx*cy*cz+sy*sz}};back=(mh3_vec3){{cx*sy,-sx,cx*cy}};
    }
    for (i=0;i<3;i++) {view.v[0][i]=right.v[i];view.v[1][i]=up.v[i];view.v[2][i]=back.v[i];}
    view.v[0][3]=-dot(&c->position,&right);view.v[1][3]=-dot(&c->position,&up);view.v[2][3]=-dot(&c->position,&back);
    if (!finite_values(&view.v[0][0],12)) return MH3_RES_INVALID;
    c->view=view;c->flags|=8;return MH3_RES_OK;
}
mh3_res_status mh3_camera_store_matrices(mh3_res_mut_span record,const mh3_native_camera *c) {
    size_t i;if (!record.data || !c) return MH3_RES_INVALID;
    if (record.size<116) return MH3_RES_RANGE;
    for (i=0;i<12;i++) putfloat(record.data+i*4,c->view.v[i/4][i%4]);
    for (i=0;i<16;i++) putfloat(record.data+48+i*4,c->projection[i]);
    put32(record.data+112,c->flags);return MH3_RES_OK;
}
mh3_res_status mh3_camera_project(mh3_native_camera *c,const mh3_vec3 *point,mh3_vec3 *out) {
    mh3_vec3 view,result;float x,y,z,w;size_t i;mh3_res_status status;
    if (!c || !point || !out) return MH3_RES_INVALID;
    status=mh3_camera_prepare_view(c);if (status) return status;status=mh3_camera_prepare_projection(c);if (status) return status;
    for (i=0;i<3;i++) view.v[i]=(c->view.v[i][0]*point->v[0]+c->view.v[i][1]*point->v[1])+c->view.v[i][2]*point->v[2]+c->view.v[i][3];
    x=view.v[0]*c->projection[0]+(c->projection_type==0?view.v[2]*c->projection[2]:c->projection[3]);
    y=view.v[1]*c->projection[5]+(c->projection_type==0?view.v[2]*c->projection[6]:c->projection[7]);
    z=view.v[2]*c->projection[10]+c->projection[11];w=c->projection_type==0?-view.v[2]:1.0f;
    if (w==0.0f) return MH3_RES_INVALID;
    result.v[0]=c->viewport[0]+c->viewport[2]*0.5f+(x/w)*c->viewport[2]*0.5f;
    result.v[1]=c->viewport[1]+c->viewport[3]*0.5f-(y/w)*c->viewport[3]*0.5f;
    result.v[2]=c->viewport[5]+(z/w)*(c->viewport[5]-c->viewport[4]);
    if (!finite_values(result.v,3)) return MH3_RES_INVALID;
    *out=result;return MH3_RES_OK;
}
mh3_res_status mh3_scene_camera_select(mh3_res_span scene,uint32_t address,int split,int player,mh3_res_span ids,uint32_t *out) {
    uint32_t index;size_t offset;
    if (!scene.data || !out) return MH3_RES_INVALID;
    if (scene.size<248) return MH3_RES_RANGE;
    if (split) {
        if (!ids.data || player<0 || (size_t)player>=ids.size) return MH3_RES_RANGE;
        index=ids.data[player];
    } else index=scene.data[244];
    if (index>=32) return MH3_RES_RANGE;
    offset=248+(size_t)index*268;
    if (offset>scene.size || 268>scene.size-offset || offset>UINT32_MAX-address) return MH3_RES_RANGE;
    *out=address+(uint32_t)offset;return MH3_RES_OK;
}
mh3_res_status mh3_camera_effective_scissor(const mh3_native_camera *c,int32_t out[4]) {
    int32_t rectangle[4];size_t axis;int shift;
    if (!c || !out) return MH3_RES_INVALID;
    for (axis=0;axis<2;axis++) {
        uint32_t origin=c->scissor[axis],extent=c->scissor[axis+2],raw=c->scissor_offset[axis];
        int64_t offset=raw<=INT32_MAX?(int64_t)raw:(int64_t)raw-INT64_C(4294967296);
        float low=c->viewport[axis],high=low+c->viewport[axis+2];int64_t start,end;
        if (!isfinite(low) || !isfinite(high) || low<0 || high>1024 || high<=low ||
            !extent || extent>1024 || (uint64_t)origin+extent+341>2047 || offset< -342 || offset>1705)
            return MH3_RES_UNSUPPORTED;
        offset=2*((offset+342)/2)-342;start=(int64_t)origin-offset;end=start+extent;
        for (shift=-4;shift<=4;shift++) if (shift &&
            (double)(end+(int64_t)shift*1024)>low && (double)(start+(int64_t)shift*1024)<high)
            return MH3_RES_UNSUPPORTED;
        rectangle[axis]=(int32_t)start;rectangle[axis+2]=(int32_t)extent;
    }
    memcpy(out,rectangle,sizeof rectangle);return MH3_RES_OK;
}
mh3_res_status mh3_scene_view_matrix(mh3_res_span state,mh3_matrix34 *out) {
    mh3_matrix34 view;uint16_t selected;size_t offset,i;
    if (!state.data || !out) return MH3_RES_INVALID;
    if (state.size<4) return MH3_RES_RANGE;
    selected=be16(state.data+2);if (selected>=32) return MH3_RES_RANGE;
    offset=52+(size_t)selected*48;
    if (offset>state.size || 48>state.size-offset) return MH3_RES_RANGE;
    for (i=0;i<12;i++) view.v[i/4][i%4]=real(state.data+offset+i*4);
    if (!finite_values(&view.v[0][0],12)) return MH3_RES_INVALID;
    *out=view;return MH3_RES_OK;
}
mh3_res_status mh3_model_cache_invalidate(mh3_model_cache_storage s,uint32_t flags) {
    static const uint32_t masks[]={1,2,5,4,8,16,32,64,64,128,256,512,1024,UINT32_MAX};
    static const size_t sizes[]={1,2,4,4,12,12,1,32,4,4,4,28,12,1};
    const mh3_res_mut_span spans[]={s.texture_valid,s.palette_valid,s.texture_scale,s.tev,s.genmode,
        s.channel,s.current_matrix,s.texture_sources,s.post_texture,s.vertex_descriptor,
        s.fog,s.light_cache,s.view_sources,s.sync};
    size_t i;
    for (i=0;i<14;i++) if (i==13 || (flags&masks[i])) {
        if (!spans[i].data) return MH3_RES_INVALID;
        if (spans[i].size<sizes[i]) return MH3_RES_RANGE;
    }
    if (flags&1) s.texture_valid.data[0]=0;
    if (flags&2) put16(s.palette_valid.data,0);
    if (flags&5) put32(s.texture_scale.data,0);
    if (flags&4) put32(s.tev.data,0);
    if (flags&8) {
        s.genmode.data[0]=1;s.genmode.data[1]=0;s.genmode.data[2]=1;s.genmode.data[3]=0;
        put32(s.genmode.data+4,2);put32(s.genmode.data+8,0);
    }
    if (flags&16) memset(s.channel.data,0,12);
    if (flags&32) s.current_matrix.data[0]=0;
    if (flags&64) {memset(s.texture_sources.data,0,32);put32(s.post_texture.data,0);}
    if (flags&128) put32(s.vertex_descriptor.data,be32(s.vertex_descriptor.data)&~1u);
    if (flags&256) put32(s.fog.data,0);
    if (flags&512) {
        put32(s.light_cache.data,UINT32_MAX);memset(s.light_cache.data+4,0,16);
        memset(s.light_cache.data+20,255,8);
    }
    if (flags&1024) memset(s.view_sources.data,0,12);
    s.sync.data[0]=1;return MH3_RES_OK;
}
mh3_res_status mh3_light_slots_initial(const mh3_light *lights,size_t count,mh3_native_light_slots *out) {
    mh3_native_light_slots x;size_t i,slot=0;if (!lights || !out || count>128) return MH3_RES_INVALID;
    memset(&x,0,sizeof x);memset(x.source_ids,-1,sizeof x.source_ids);x.ambient=(mh3_rgba8){255,255,255,255};
    for (i=0;i<count && slot<8;i++) if (lights[i].flags&MH3_LIGHT_ENABLED) {x.lights[slot]=lights[i];x.source_ids[slot]=(int8_t)i;x.valid_mask|=(uint8_t)(1u<<slot);slot++;}
    *out=x;return MH3_RES_OK;
}
mh3_res_status mh3_light_slots_select(const mh3_light *lights,size_t count,const mh3_light_set *set,const mh3_rgba8 *ambient,size_t ambient_count,mh3_native_light_slots *slots) {
    mh3_native_light_slots x;size_t i,j,k;uint32_t bit;
    if (!lights || !slots || count>128 || ambient_count>128) return MH3_RES_INVALID;
    x=*slots;x.diffuse_color=x.diffuse_alpha=x.specular_color=x.specular_alpha=0;x.ambient=(mh3_rgba8){255,255,255,255};
    if (!set) {*slots=x;return MH3_RES_OK;}
    if (set->ambient>=0) {if (!ambient || (size_t)set->ambient>=ambient_count) return MH3_RES_RANGE;x.ambient=ambient[(size_t)set->ambient];}
    for (i=0;i<8;i++) {
        int id=set->light[i];if (id<0) continue;if ((size_t)id>=count) return MH3_RES_RANGE;
        if (!(lights[id].flags&MH3_LIGHT_ENABLED)) continue;
        for (j=0;j<8 && x.source_ids[j]!=id;j++) {}
        if (j==8) for (j=0;j<8 && x.source_ids[j]!=-1;j++) {}
        if (j==8) {
            for (j=0;j<8;j++) {for (k=0;k<8 && set->light[k]!=x.source_ids[j];k++) {}if (k==8) break;}
            if (j==8) return MH3_RES_INVALID;
        }
        /* A resident slot is not reloaded by80085758; preserve its snapshot. */
        if (x.source_ids[j]!=id) x.lights[j]=lights[id];
        x.source_ids[j]=(int8_t)id;x.valid_mask|=(uint8_t)(1u<<j);bit=1u<<j;
        if (lights[id].flags&MH3_LIGHT_SPECULAR) {
            if (!(lights[id].flags&MH3_LIGHT_DISABLE_COLOR)) x.specular_color|=bit;
            if (!(lights[id].flags&MH3_LIGHT_DISABLE_ALPHA)) x.specular_alpha|=bit;
        } else {
            if (!(lights[id].flags&MH3_LIGHT_DISABLE_COLOR)) x.diffuse_color|=bit;
            if (!(lights[id].flags&MH3_LIGHT_DISABLE_ALPHA)) x.diffuse_alpha|=bit;
        }
    }
    *slots=x;return MH3_RES_OK;
}
static mh3_res_status light_state_read(mh3_res_span state,mh3_light lights[128],uint16_t *count,uint16_t *sets) {
    size_t i;
    if (!state.data || state.size<MH3_LIGHT_STATE_BYTES) return MH3_RES_RANGE;
    *count=be16(state.data);*sets=be16(state.data+2);
    if (*count>128 || *sets>128) return MH3_RES_RANGE;
    for (i=0;i<*count;i++) {
        mh3_res_status status=mh3_light_read((mh3_res_span){state.data+44+i*68,68},lights+i);
        if (status) return status;
    }
    return MH3_RES_OK;
}
mh3_res_status mh3_light_state_import(mh3_res_mut_span state,uint16_t count,uint16_t sets,mh3_res_span input,
    mh3_res_span ambient,mh3_res_span input_sets,const mh3_matrix34 *view,mh3_native_light_slots *out) {
    uint8_t bytes[MH3_LIGHT_STATE_BYTES];mh3_light lights[128];mh3_native_light_slots slots;
    uint16_t capacity,set_capacity;size_t i;mh3_res_status status;
    if (!state.data || !view || !out) return MH3_RES_INVALID;
    if (state.size<sizeof bytes || count>128 || sets>128 ||
        input.size<(size_t)count*68 || ambient.size<(size_t)count*4 || input_sets.size<(size_t)sets*12) return MH3_RES_RANGE;
    if ((count && (!input.data || !ambient.data)) || (sets && !input_sets.data)) return MH3_RES_INVALID;
    capacity=be16(state.data);set_capacity=be16(state.data+2);
    if (capacity>128 || set_capacity>128 || count>capacity || sets>set_capacity) return MH3_RES_RANGE;
    memcpy(bytes,state.data,sizeof bytes);
    for (i=0;i<count;i++) {
        uint8_t *destination=bytes+44+i*68;
        if (be32(input.data+i*68)&MH3_LIGHT_ENABLED) memmove(destination,input.data+i*68,68);
        else put32(destination,be32(destination)&~(uint32_t)MH3_LIGHT_ENABLED);
        memcpy(bytes+0x222c+i*4,ambient.data+i*4,4);
    }
    if (sets) memmove(bytes+0x242c,input_sets.data,(size_t)sets*12);
    status=light_state_read((mh3_res_span){bytes,sizeof bytes},lights,&capacity,&set_capacity);if (status) return status;
    for (i=0;i<capacity;i++) if (lights[i].flags&MH3_LIGHT_ENABLED) {
        mh3_light_apply_view(lights+i,view);
        status=mh3_light_store((mh3_res_mut_span){bytes+44+i*68,68},lights+i);if (status) return status;
    }
    status=mh3_light_slots_initial(lights,capacity,&slots);if (status) return status;
    put32(bytes+16,UINT32_MAX);memset(bytes+20,0,16);memcpy(bytes+36,slots.source_ids,8);
    memcpy(state.data,bytes,sizeof bytes);*out=slots;return MH3_RES_OK;
}
mh3_res_status mh3_light_state_select(mh3_res_mut_span state,uint32_t id,mh3_native_light_slots *slots) {
    mh3_light lights[128];mh3_native_light_slots x;mh3_light_set set;mh3_rgba8 ambient[128];
    uint16_t count,sets;size_t i;mh3_res_status status;
    if (!state.data || !slots) return MH3_RES_INVALID;
    x=*slots;
    if (id>=128) {x.diffuse_color=x.diffuse_alpha=x.specular_color=x.specular_alpha=0;x.ambient=(mh3_rgba8){255,255,255,255};*slots=x;return MH3_RES_OK;}
    status=light_state_read((mh3_res_span){state.data,state.size},lights,&count,&sets);if (status) return status;
    /* LightState owns128 set/ambient records independent of source capacity. */
    memcpy(set.light,state.data+0x242c+(size_t)id*12,8);memcpy(&set.ambient,state.data+0x2434+(size_t)id*12,1);
    memcpy(ambient,state.data+0x222c,sizeof ambient);
    x.ambient=set.ambient<0?(mh3_rgba8){255,255,255,255}:ambient[(uint8_t)set.ambient];
    if (be32(state.data+16)==id) {
        x.diffuse_color=be32(state.data+20);x.diffuse_alpha=be32(state.data+24);
        x.specular_color=be32(state.data+28);x.specular_alpha=be32(state.data+32);*slots=x;return MH3_RES_OK;
    }
    memcpy(x.source_ids,state.data+36,8);x.valid_mask=0;
    for (i=0;i<8;i++) if (x.source_ids[i]>=0) x.valid_mask|=(uint8_t)(1u<<i);
    status=mh3_light_slots_select(lights,count,&set,ambient,128,&x);if (status) return status;
    put32(state.data+16,id);put32(state.data+20,x.diffuse_color);put32(state.data+24,x.diffuse_alpha);
    put32(state.data+28,x.specular_color);put32(state.data+32,x.specular_alpha);memcpy(state.data+36,x.source_ids,8);
    *slots=x;return MH3_RES_OK;
}
mh3_res_status mh3_fog_range_table(uint16_t width,const float p[16],uint16_t out[10]) {
    float reference,extent,step,ratio,value;uint16_t table[10];size_t i;
    if (!p || !out || !width || !finite_values(p,16) || p[0]==0.0f) return MH3_RES_INVALID;
    if (p[15]==0.0f) {
        if (p[10]==1.0f) return MH3_RES_INVALID;
        reference=p[11]/(p[10]-1.0f);extent=reference/p[0];
    } else {extent=1.0f/p[0];reference=1.7320499420166016f*extent;}
    if (reference==0.0f) return MH3_RES_INVALID;
    step=2.0f/(float)width;
    for (i=0;i<10;i++) {
        ratio=((float)((i+1)*32)*step)*extent;
        value=256.0f*sqrtf(1.0f+(ratio*ratio)/(reference*reference));
        if (!(value>=0.0f && value<4294967296.0f)) return MH3_RES_RANGE;
        table[i]=(uint16_t)((uint32_t)value&4095u);
    }
    memcpy(out,table,sizeof table);return MH3_RES_OK;
}
static float clamp_real(float x,float low,float high) {return x<low?low:x>high?high:x;}
mh3_res_status mh3_texture_compat_store(mh3_res_mut_span object,const mh3_native_texture_binding *b) {
    static const uint8_t min_filter[6]={0,4,1,5,2,6};
    uint8_t bytes[32]={0};uint32_t mode0,mode1,image0,block_s,block_t,load_format,blocks;float bias;
    if (!object.data || !b) return MH3_RES_INVALID;
    if (object.size<32) return MH3_RES_RANGE;
    if (!b->source.width || !b->source.height || b->source.width>1024 || b->source.height>1024 ||
        b->sampler.wrap_s>2 || b->sampler.wrap_t>2 || b->sampler.min_filter>5 || b->sampler.mag_filter>1 ||
        b->sampler.anisotropy>2 || !isfinite(b->source.min_lod) || !isfinite(b->source.max_lod) || !isfinite(b->sampler.lod_bias)) return MH3_RES_INVALID;
    switch (b->source.format&15) {
    case 0:case 8:block_s=3;block_t=3;load_format=1;break;
    case 1:case 2:case 9:block_s=3;block_t=2;load_format=2;break;
    case 3:case 4:case 5:case 10:block_s=2;block_t=2;load_format=2;break;
    case 6:block_s=2;block_t=2;load_format=3;break;
    case 14:block_s=3;block_t=3;load_format=0;break;
    default:return MH3_RES_UNSUPPORTED;
    }
    bias=clamp_real(b->sampler.lod_bias,-4.0f,3.9900000095367432f);
    mode0=b->sampler.wrap_s|(b->sampler.wrap_t<<2)|(b->sampler.mag_filter<<4)|((uint32_t)min_filter[b->sampler.min_filter]<<5)|
        ((uint32_t)!b->sampler.edge_lod<<8)|(((uint32_t)(int32_t)(bias*32.0f)&255u)<<9)|
        (b->sampler.anisotropy<<19)|((uint32_t)(b->sampler.bias_clamp!=0)<<21);
    mode1=(uint32_t)(clamp_real(b->source.min_lod,0.0f,10.0f)*16.0f)|((uint32_t)(clamp_real(b->source.max_lod,0.0f,10.0f)*16.0f)<<8);
    image0=(b->source.width-1)|((b->source.height-1)<<10)|((b->source.format&15)<<20);
    put32(bytes,mode0);put32(bytes+4,mode1);put32(bytes+8,image0);put32(bytes+12,(b->source.payload_address>>5)&0xffffffu);
    put32(bytes+20,b->source.format);put32(bytes+24,b->source.indexed?b->tlut_id:0);
    blocks=((b->source.width+(1u<<block_s)-1)>>block_s)*((b->source.height+(1u<<block_t)-1)>>block_t);
    put16(bytes+28,(uint16_t)(blocks&32767));bytes[30]=(uint8_t)load_format;
    bytes[31]=(uint8_t)((b->source.levels>1?1:0)|(b->source.indexed?0:2));memcpy(object.data,bytes,32);return MH3_RES_OK;
}
mh3_res_status mh3_palette_compat_store(mh3_res_mut_span object,const mh3_native_palette_source *palette) {
    if (!object.data || !palette) return MH3_RES_INVALID;
    if (object.size<12 || palette->entries>65535) return MH3_RES_RANGE;
    if (palette->format>2) return MH3_RES_INVALID;
    put32(object.data,palette->format<<10);put32(object.data+4,0x64000000u|((palette->payload_address>>5)&0xffffffu));
    put16(object.data+8,(uint16_t)palette->entries);return MH3_RES_OK;
}
