#include "mh3_owned_scene.h"
#include "mh3_material_state.h"
#include <assert.h>
#include <math.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint16_t be16(const uint8_t *p) {return (uint16_t)((uint16_t)p[0]<<8|p[1]);}
static float number(const uint8_t *p) {uint32_t bits=be32(p);float v;memcpy(&v,&bits,4);return v;}
static int fits(const mh3_res_model *m,size_t at,size_t bytes) {
    return at>=m->offset && at-m->offset<=m->size && bytes<=m->size-(at-m->offset) &&
        at<=m->container.size && bytes<=m->container.size-at;
}
static int defined(const mh3_owned_model *m,uint32_t matrix) {
    if(!matrix) return 1;
    for(uint32_t i=0;i<m->tree_count;i++) {
        const mh3_owned_tree *t=&m->tree[i];
        if((t->operation==2?m->locals[t->destination].matrix:t->destination)==matrix) return 1;
    }
    return 0;
}
static mh3_res_status static_local(const mh3_res_model *m,uint32_t id,mh3_owned_local *out) {
    mh3_res_entry entry;mh3_res_status s=mh3_res_model_entry(m,MH3_RES_NODES,id,&entry);if(s) return s;
    if(!fits(m,entry.offset,0xd0)) return MH3_RES_RANGE;
    const uint8_t *p=m->container.data+entry.offset;
    if(be32(p)<0xd0 || be32(p+0x10)>65535u) return MH3_RES_RANGE;
    const uint32_t flags=be32(p+0x14);
    /* ROT_ZERO, no SSC flags, no billboard-parent rewrite. No angle->matrix
     * approximation or substitution of the node's global bind matrix. */
    /* Static Chr result maps source0x20/0x40 into its SSC control bits.
     * View preparation consumes source0x400 for billboard-parent rewriting.
     * Source0x200 is not consumed by these matrix paths and remains legal. */
    if(!(flags&4u) || (flags&0x460u) || be32(p+0x18)) return MH3_RES_UNSUPPORTED;
    mh3_owned_local local={0};local.rt=(mh3_matrix34){{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
    local.matrix=(uint16_t)be32(p+0x10);local.flags=0x20u;
    if(flags&8u) {local.scale=(mh3_vec3){{1,1,1}};local.flags|=8u|0x10u;}
    else {
        for(unsigned k=0;k<3;k++) local.scale.v[k]=number(p+0x20+k*4);
        if(flags&0x10u) local.flags|=0x10u;
    }
    if(flags&2u) {local.flags|=0x40u|4u;if(flags&8u) local.flags|=2u;}
    else for(unsigned k=0;k<3;k++) local.rt.v[k][3]=number(p+0x38+k*4);
    *out=local;return MH3_RES_OK;
}
static mh3_res_status compile_draws(const mh3_res_model *m,const char *name,unsigned pass,
    const mh3_owned_model_storage *storage,mh3_owned_model *out,uint32_t *used) {
    mh3_res_entry code;mh3_res_status s=mh3_res_model_find(m,MH3_RES_BYTECODE,name,&code);
    out->draw_first[pass]=*used;
    if(s==MH3_RES_ABSENT) return MH3_RES_OK;
    if(s) return s;
    size_t cursor=code.offset;
    for(;;) {
        if(!fits(m,cursor,1)) return MH3_RES_RANGE;
        if(m->container.data[cursor]!=1 && m->container.data[cursor]!=4) return MH3_RES_UNSUPPORTED;
        mh3_res_draw draw;s=mh3_res_draw_next(m,&cursor,&draw);if(s==MH3_RES_END) break;if(s) return s;
        if(draw.node_id>=out->local_count || draw.material_id>=out->material_count || draw.shape_id>=out->shape_count)
            return MH3_RES_RANGE;
        mh3_res_entry node,material;mh3_res_shape shape;
        s=mh3_res_model_entry(m,MH3_RES_NODES,draw.node_id,&node);if(s) return s;
        s=mh3_res_model_entry(m,MH3_RES_MATERIALS,draw.material_id,&material);if(s) return s;
        s=mh3_res_shape_open(m,draw.shape_id,&shape);if(s) return s;
        if(!fits(m,node.offset,0x18) || !fits(m,material.offset,0x10) || material.offset>UINT32_MAX) return MH3_RES_RANGE;
        if(!(be32(m->container.data+node.offset+0x14)&0x100u)) continue;
        if(*used>=storage->draw_capacity || out->draw_count[pass]>=MH3_MODEL_SORT_CAPACITY-1u) return MH3_RES_RANGE;
        if(shape.matrix_id<0 || (uint32_t)shape.matrix_id>=out->view_count) return MH3_RES_UNSUPPORTED;
        mh3_owned_draw *record=&storage->draws[(*used)++];
        *record=(mh3_owned_draw){0};record->record.node=draw.node_id;record->record.material=draw.material_id;
        record->record.shape=draw.shape_id;record->record.priority=draw.priority;
        ++out->draw_count[pass];
    }
    return MH3_RES_OK;
}
mh3_res_status mh3_owned_model_compile(const mh3_res_model *m,const mh3_owned_model_storage *storage,
    const mh3_owned_material *materials,uint32_t material_count,const mh3_prepared_mesh *meshes,
    uint32_t shape_count,uint64_t generation,mh3_owned_model *out) {
    if(!m || !storage || !out || !generation || !storage->locals || !storage->tree || !storage->draws ||
       (!materials && material_count) || (!meshes && shape_count)) return MH3_RES_INVALID;
    if(m->version!=11 || !fits(m,m->offset+0x4c,0x28)) return MH3_RES_UNSUPPORTED;
    const uint8_t *info=m->container.data+m->offset+0x4c;
    if(be32(info+8)!=1u) return MH3_RES_UNSUPPORTED;
    mh3_owned_model result={0};result.locals=storage->locals;result.tree=storage->tree;result.draws=storage->draws;
    result.materials=materials;result.meshes=meshes;result.generation=generation;
    result.shapes=storage->shapes;result.material_keys=storage->material_keys;
    size_t dictionary;mh3_res_status s=mh3_res_model_dictionary(m,MH3_RES_NODES,&dictionary,&result.local_count);
    if(s) return s;
    if(!result.local_count || result.local_count>storage->local_capacity) return MH3_RES_RANGE;
    uint32_t actual_materials=0,actual_shapes=0;
    s=mh3_res_model_dictionary(m,MH3_RES_MATERIALS,&dictionary,&actual_materials);if(s && s!=MH3_RES_ABSENT) return s;
    s=mh3_res_model_dictionary(m,MH3_RES_SHAPES,&dictionary,&actual_shapes);if(s && s!=MH3_RES_ABSENT) return s;
    if(actual_materials!=material_count || actual_shapes!=shape_count || material_count>storage->material_capacity ||
       shape_count>storage->shape_capacity || (material_count && !storage->material_keys) || (shape_count && !storage->shapes)) return MH3_RES_RANGE;
    result.material_count=material_count;result.shape_count=shape_count;
    result.view_count=be32(info+0x1c);result.matrix_count=result.view_count;
    if(!result.view_count || result.view_count>65535u) return MH3_RES_RANGE;
    for(uint32_t i=0;i<result.local_count;i++) {s=static_local(m,i,&storage->locals[i]);if(s) return s;}
    for(uint32_t i=0;i<material_count;i++) {
        mh3_res_entry entry;s=mh3_res_model_entry(m,MH3_RES_MATERIALS,i,&entry);if(s) return s;
        if(entry.offset>UINT32_MAX) return MH3_RES_RANGE;
        storage->material_keys[i]=(uint32_t)entry.offset;
        if(!materials[i].publication || (materials[i].top_known&~MH3_MATERIAL_TOP_ALL)) return MH3_RES_INVALID;
        s=mh3_material_state_validate(materials[i].publication,NULL,NULL);if(s) return s;
        const mh3_native_material_state *state=materials[i].publication;
        /* A prior producer may have enabled textures or lighting. These counts
         * must be authored writes in this untextured/unlit subset, rather than
         * unspecified zero bytes which would inherit those earlier bindings. */
        const uint32_t counts=MH3_MATERIAL_TOP_TEXGEN_COUNT|MH3_MATERIAL_TOP_CHANNEL_COUNT|MH3_MATERIAL_TOP_INDIRECT_COUNT;
        if((materials[i].top_known&counts)!=counts) return MH3_RES_UNSUPPORTED;
        if(state->texgen_count || state->indirect_count || state->light_set_reference!=-1 || state->fog_reference!=-1)
            return MH3_RES_UNSUPPORTED;
        for(uint32_t channel=0;channel<state->channel_count;channel++) {
            const mh3_pack_channel *value=&state->channel[channel];
            const uint32_t controls=MH3_CHANNEL_CONTROL_COLOR|MH3_CHANNEL_CONTROL_ALPHA;
            if((value->flags&controls)!=controls || value->color.lighting || value->alpha.lighting)
                return MH3_RES_UNSUPPORTED;
        }
    }
    for(uint32_t i=0;i<shape_count;i++) {
        if(meshes[i].palette_batch_count || meshes[i].matrix_index_mask || !meshes[i].immutable.resident_generation) return MH3_RES_UNSUPPORTED;
        mh3_res_shape shape;s=mh3_res_shape_open(m,i,&shape);if(s) return s;
        if(shape.matrix_id<0 || (uint32_t)shape.matrix_id>=result.view_count) return MH3_RES_UNSUPPORTED;
        storage->shapes[i]=(mh3_owned_shape){shape.matrix_id,(shape.flags&2u)!=0};
    }
    mh3_res_entry code;s=mh3_res_model_find(m,MH3_RES_BYTECODE,"NodeMix",&code);
    if(s==MH3_RES_OK && (!fits(m,code.offset,1) || m->container.data[code.offset]!=1)) return MH3_RES_UNSUPPORTED;
    if(s!=MH3_RES_OK && s!=MH3_RES_ABSENT) return s;
    s=mh3_res_model_find(m,MH3_RES_BYTECODE,"NodeTree",&code);if(s) return s;
    for(size_t cursor=code.offset;;cursor+=5u) {
        if(!fits(m,cursor,1)) return MH3_RES_RANGE;
        const uint8_t op=m->container.data[cursor];if(op==1) break;
        if(op!=2 && op!=6) return MH3_RES_UNSUPPORTED;
        if(!fits(m,cursor,5) || result.tree_count>=storage->tree_capacity) return MH3_RES_RANGE;
        const uint16_t a=be16(m->container.data+cursor+1),b=be16(m->container.data+cursor+3);
        if((op==2 && a>=result.local_count) || !defined(&result,b)) return MH3_RES_RANGE;
        const uint32_t destination=op==2?result.locals[a].matrix:a;
        if(destination>=65535u) return MH3_RES_RANGE;
        if(destination>=result.matrix_count) result.matrix_count=destination+1u;
        storage->tree[result.tree_count++]=(mh3_owned_tree){a,b,op};
    }
    for(uint32_t i=0;i<result.view_count;i++) if(!defined(&result,i)) return MH3_RES_RANGE;
    uint32_t used=0;s=compile_draws(m,"DrawOpa",0,storage,&result,&used);if(s) return s;
    s=compile_draws(m,"DrawXlu",1,storage,&result,&used);if(s) return s;
    *out=result;return MH3_RES_OK;
}
void mh3_owned_scene_release(mh3_owned_scene_packet *packet) {
    assert(packet);
    for(uint32_t i=0;i<packet->pinned_objects;i++) packet->pins.release(packet->pins.user,packet->objects[i].model);
    packet->pinned_objects=0;packet->admitted=0;
}
mh3_res_status mh3_owned_scene_capture(mh3_owned_scene_packet *packet,const mh3_owned_scene_input *source,
    const mh3_owned_asset_pins *pins) {
    if(!packet || !source || !pins || !pins->retain || !pins->release || !source->generation || packet->admitted)
        return MH3_RES_INVALID;
    const uint64_t members=(uint64_t)source->member_count[0]+source->member_count[1];
    if(members>packet->member_capacity || members>UINT32_C(0x1000000)/52u-80u ||
       source->object_count>packet->object_capacity || (members && !packet->members) ||
       (source->object_count && (!source->objects || !packet->objects)) || source->filter>1)
        return MH3_RES_RANGE;
    uint64_t matrices=0;uint32_t buckets=0;
    for(unsigned category=0;category<6;category++) {
        if(!source->bucket_count[category]) return MH3_RES_RANGE;
        buckets+=source->bucket_count[category];
    }
    for(uint32_t i=0;i<source->object_count;i++) {
        const mh3_owned_object_input *object=&source->objects[i];
        if(object->unsupported || !object->model || !object->model->generation || !object->source_generation ||
           (object->options&~3u) || (object->root_flags&255u)) return MH3_RES_UNSUPPORTED;
        matrices+=object->model->matrix_count;if(matrices>UINT32_MAX) return MH3_RES_RANGE;
    }
    for(unsigned set=0;set<2;set++) {
        if(source->member_count[set] && !source->members[set]) return MH3_RES_INVALID;
        for(uint32_t i=0;i<source->member_count[set];i++) {
            const mh3_owned_member *member=&source->members[set][i];
            if(member->node.type>1 || member->node.callback || member->node.category>=6 ||
               member->object_index>=source->object_count) return MH3_RES_UNSUPPORTED;
        }
    }
    /* Admission above completes before either payload publication or pinning. */
    packet->pins=*pins;packet->pinned_objects=0;
    for(uint32_t i=0;i<source->object_count;i++) {
        packet->objects[i]=source->objects[i];
        const mh3_res_status s=pins->retain(pins->user,packet->objects[i].model);
        if(s) {mh3_owned_scene_release(packet);return s;}
        ++packet->pinned_objects;
    }
    uint32_t index=0;
    for(unsigned set=0;set<2;set++) for(uint32_t i=0;i<source->member_count[set];i++,index++) {
        packet->members[index]=source->members[set][i];
        packet->members[index].node.identity=UINT32_C(0x1000)+index*52u;
        packet->members[index].node.object=packet->members[index].object_index+1u;
        packet->members[index].node.initialized=0;
    }
    packet->input=*source;packet->input.objects=packet->objects;
    packet->input.members[0]=packet->members;
    packet->input.members[1]=packet->members?packet->members+source->member_count[0]:NULL;
    packet->matrix_count=(uint32_t)matrices;packet->bucket_total=buckets;packet->admitted=1;return MH3_RES_OK;
}
/* Ordinary native float policy shared with the verified view kernel. */
static mh3_matrix34 concat(const mh3_matrix34 *a,const mh3_matrix34 *b) {
    mh3_matrix34 result;const uint32_t attribute=0;
    const mh3_model_view_native_input input={1,*a,b,&attribute,&result,NULL,NULL};
    mh3_model_view_native_static(&input);return result;
}
static uint32_t maya(const mh3_owned_local *local,const mh3_matrix34 *parent,const mh3_vec3 *scale,
    uint32_t flags,mh3_matrix34 *world,mh3_vec3 *world_scale) {
    if(local->flags&6u) *world=*parent;
    else if(local->flags&0x20u) {
        *world=*parent;float translation[3];
        for(unsigned k=0;k<3;k++) translation[k]=(flags&UINT32_C(0x80000000))?local->rt.v[k][3]:scale->v[k]*local->rt.v[k][3];
        for(unsigned r=0;r<3;r++) world->v[r][3]=fmaf(parent->v[r][2],translation[2],parent->v[r][0]*translation[0])+
            fmaf(parent->v[r][3],1.0f,parent->v[r][1]*translation[1]);
    } else {
        mh3_matrix34 rt=local->rt;
        if(!(flags&UINT32_C(0x80000000))) for(unsigned k=0;k<3;k++) rt.v[k][3]*=scale->v[k];
        *world=concat(parent,&rt);
    }
    if(local->flags&8u) {flags|=UINT32_C(0x40000000);*world_scale=*scale;}
    else {flags&=UINT32_C(0x3fffffff);for(unsigned k=0;k<3;k++) world_scale->v[k]=scale->v[k]*local->scale.v[k];}
    return local->flags&0x10u?flags|UINT32_C(0x10000000):flags&UINT32_C(0x0fffffff);
}
typedef struct render {
    mh3_owned_scene_packet *packet;mh3_owned_scene_work *work;mh3_owned_render_state *state;
    mh3_owned_scene_emit emit;void *user;mh3_owned_scene_stats stats;
    uint32_t heads[6],current_object,current_member;int translucent;
} render;
static uint32_t node_index(render *r,uint32_t id) {
    (void)r;
    assert(id>=0x1000u && !((id-0x1000u)%52u));
    const uint32_t index=(id-0x1000u)/52u;
    assert(index<r->packet->input.member_count[0]+r->packet->input.member_count[1]);return index;
}
static uint32_t link_read(void *p,uint32_t id) {
    render *r=p;if(id>=UINT32_C(0x1000000)) {assert((id&3u)==0);return r->work->buckets[(id-0x1000000u)/4u];}
    return r->work->links[node_index(r,id)];
}
static void link_write(void *p,uint32_t id,uint32_t value) {
    render *r=p;if(id>=UINT32_C(0x1000000)) {assert((id&3u)==0);r->work->buckets[(id-0x1000000u)/4u]=value;}
    else r->work->links[node_index(r,id)]=value;
}
static float link_depth(void *p,uint32_t id) {render *r=p;return r->packet->members[node_index(r,id)].node.depth;}
static uint32_t count(void *p,unsigned set) {render *r=p;return r->packet->input.member_count[set];}
static uint32_t identity(void *p,unsigned set,uint32_t index) {
    render *r=p;return 0x1000u+(index+(set?r->packet->input.member_count[0]:0))*52u;
}
static void read_node(void *p,uint32_t id,mh3_scene_node *out) {render *r=p;*out=r->packet->members[node_index(r,id)].node;}
static void phase(void *p,uint32_t id,uint8_t value) {render *r=p;r->packet->members[node_index(r,id)].node.phase=value;}
static void initialized(void *p,uint32_t id) {render *r=p;r->packet->members[node_index(r,id)].node.initialized=1;}
static int query(void *p,unsigned kind) {render *r=p;assert(kind<4);return r->packet->input.query[kind];}
static void callback(void *p,uint32_t fn,uint32_t id) {(void)p;(void)fn;(void)id;assert(!"Unsupported callback passed cold admission");}
static void prepare(void *p,uint32_t object) {
    render *r=p;assert(object && object<=r->packet->input.object_count);const uint32_t index=object-1u;
    mh3_owned_object_work *work=&r->work->objects[index];if(work->prepared) return;
    const mh3_owned_object_input *input=&r->packet->objects[index];const mh3_owned_model *m=input->model;
    mh3_matrix34 *world=r->work->world+work->matrix_first;mh3_vec3 *scale=r->work->scale+work->matrix_first;
    uint32_t *flags=r->work->attributes+work->matrix_first;
    work->world=input->local;world[0]=input->local;scale[0]=(mh3_vec3){{1,1,1}};flags[0]=input->root_flags;
    for(uint32_t i=0;i<m->tree_count;i++) {
        const mh3_owned_tree *t=&m->tree[i];
        if(t->operation==6) {world[t->destination]=world[t->source];scale[t->destination]=scale[t->source];flags[t->destination]=flags[t->source];}
        else {
            const mh3_owned_local *local=&m->locals[t->destination];
            flags[local->matrix]=maya(local,&world[t->source],&scale[t->source],flags[t->source],&world[local->matrix],&scale[local->matrix])&UINT32_C(0xffffff00);
        }
    }
    /* Preserve authored repeated destination IDs: their final scale is applied
     * once for each write, exactly as CalcWorld's matrix-ID list does. */
    for(uint32_t i=0;i<m->tree_count;i++) {
        const mh3_owned_tree *t=&m->tree[i];const uint32_t id=t->operation==2?m->locals[t->destination].matrix:t->destination;
        if(scale[id].v[0]!=1.0f || scale[id].v[1]!=1.0f || scale[id].v[2]!=1.0f)
            for(unsigned row=0;row<3;row++) for(unsigned column=0;column<4;column++) world[id].v[row][column]*=column==3?1.0f:scale[id].v[column];
    }
    work->prepared=1;++r->stats.objects_prepared;r->stats.world_matrices+=m->tree_count;
}
static void apply_view(void *p,uint32_t object,const float camera[12]) {
    render *r=p;prepare(p,object);const uint32_t index=object-1u;
    mh3_owned_object_work *work=&r->work->objects[index];const mh3_owned_model *m=r->packet->objects[index].model;
    if(work->viewed) return;
    (void)camera;work->view=concat(&r->packet->input.camera,&work->world);
    const uint32_t first=work->matrix_first;
    const mh3_model_view_native_input input={m->view_count,r->packet->input.camera,r->work->world+first,
        r->work->attributes+first,r->work->position+first,r->work->normal+first,NULL};
    mh3_model_view_native_static(&input);work->viewed=1;r->stats.view_matrices+=m->view_count;
}
static int visibility(void *p,uint32_t id,const float view[12]) {
    render *r=p;mh3_owned_member *member=&r->packet->members[node_index(r,id)];
    return mh3_scene_visible(&member->node,view,member->node.position,r->work->objects[member->object_index].view.v[2][3]);
}
static float height(void *p,uint32_t object,uint8_t type) {
    render *r=p;(void)type;assert(type<2 && object);return r->packet->objects[object-1u].local.v[2][3];
}
static void enqueue(void *p,uint32_t id,uint8_t category,float origin,float span) {
    render *r=p;uint32_t bin=0;const mh3_scene_links io={r,link_read,link_write,link_depth};
    int okay=mh3_scene_bucket(r->packet->members[node_index(r,id)].node.depth,origin,span,r->packet->input.bucket_count[category],&bin);
    assert(okay);(void)okay;okay=mh3_scene_insert(&io,r->heads[category]+bin*4u,id,category);assert(okay);(void)okay;++r->stats.visible;
}
static int less(void *p,size_t a,size_t b) {render *r=p;return mh3_model_draw_less(&r->work->sort[a],&r->work->sort[b],r->translucent);}
static void swap(void *p,size_t a,size_t b) {render *r=p;mh3_model_draw_swap(&r->work->sort[a],&r->work->sort[b]);}
static int32_t get_phase(void *p,unsigned which) {render *r=p;return r->state->sort_phase[which];}
static void set_phase(void *p,unsigned which,int32_t value) {render *r=p;r->state->sort_phase[which]=value;}
static float draw_depth(float value) {uint32_t bits;memcpy(&bits,&value,4);if((bits&0x7fffffffu)>0x7f800000u) bits|=0x400000u;memcpy(&value,&bits,4);return value;}
static mh3_res_status draw_pass(render *r,unsigned pass) {
    const mh3_owned_object_input *object=&r->packet->objects[r->current_object];const mh3_owned_model *m=object->model;
    const uint32_t first=r->work->objects[r->current_object].matrix_first,count=m->draw_count[pass];
    const int sorted=(object->options&(1u<<pass))!=0;r->translucent=(int)pass;
    if(sorted) {
        assert(count<=r->work->sort_capacity);
        for(uint32_t i=0;i<count;i++) {
            const mh3_owned_draw *d=&m->draws[m->draw_first[pass]+i];r->work->sort[i]=d->record;
            const uint32_t depth_matrix=m->locals[d->record.node].matrix;
            r->work->sort[i].depth=draw_depth(depth_matrix<m->view_count?r->work->position[first+depth_matrix].v[2][3]:r->packet->input.missing_depth);
        }
        const mh3_model_sort_ops ops={r,less,swap,get_phase,set_phase};
        const mh3_res_status s=mh3_model_draw_sort(&ops,0,count,0);assert(s==MH3_RES_OK);(void)s;
    }
    uint32_t previous=0;int has_previous=0;
    for(uint32_t i=0;i<count;i++) {
        const mh3_model_draw_record *record=sorted?&r->work->sort[i]:&m->draws[m->draw_first[pass]+i].record;
        const mh3_owned_shape *shape_info=&m->shapes[record->shape];
        const uint32_t material_key=m->material_keys[record->material];
        if(shape_info->hidden) continue;
        uint32_t flags=1u;
        if(has_previous && previous==material_key) {flags|=16u;++r->stats.repeated_materials;}
        if(!(flags&16u)) {
            const mh3_owned_material *material=&m->materials[record->material];
            mh3_material_state_overlay_unchecked(&r->state->material,material->publication,material->top_known);
            ++r->stats.material_publications;
        }
        const mh3_prepared_mesh *mesh=&m->meshes[record->shape];
        const uint32_t matrix_id=(uint32_t)shape_info->matrix;
        assert(matrix_id<m->view_count);
#ifndef NDEBUG
        /* Existing checked APIs remain a debug oracle. Cold asset and packet
         * admission already prove the hot binding contracts. */
        const mh3_matrix_sources sources={{{(const uint8_t *)(r->work->position+first),(size_t)m->view_count*sizeof(mh3_matrix34)},m->view_count,sizeof(mh3_matrix34),0},
            {{(const uint8_t *)(r->work->normal+first),(size_t)m->view_count*sizeof(mh3_matrix33)},m->view_count,sizeof(mh3_matrix33),0},{0},r->packet->input.generation};
        mh3_shape_matrix_input shape={0};shape.valid=1;shape.matrix_id=shape_info->matrix;shape.has_normal=mesh->normal_required;
        shape.current_is_valid=1;shape.view_array_is_valid=1;
        mh3_model_palette produced,expected;
        mh3_res_status oracle=mh3_model_palette_rigid(&shape,&sources,&produced);assert(oracle==MH3_RES_OK);
        oracle=mh3_model_palette_overlay(&r->state->palette,&produced,&expected);assert(oracle==MH3_RES_OK);
#endif
        /* Rigid, untextured effective loads write slot0 only. Other PN/normal/
         * texture slots retain sticky contents; all eight row selectors are
         * authored identity (60). No whole-palette zero/copy/merge per draw. */
        r->state->palette.position[0]=r->work->position[first+matrix_id];r->state->palette.position_valid|=1u;
        if(mesh->normal_required) {r->state->palette.normal[0]=r->work->normal[first+matrix_id];r->state->palette.normal_valid|=1u;}
        for(unsigned i=0;i<8;i++) r->state->palette.texture_ids[i]=60;
#ifndef NDEBUG
        assert(!memcmp(&r->state->palette,&expected,sizeof expected));
#endif
        mh3_owned_scene_submission submission;submission.mesh=mesh->immutable;
        submission.mesh.source_generation=object->source_generation;submission.mesh.pose_generation=r->packet->input.generation;
        submission.mesh.palette=&r->state->palette;
#ifndef NDEBUG
        mh3_resolved_mesh_submit checked;
        const mh3_pack_status bound=mh3_model_mesh_bind_pose(mesh,mesh->immutable.resident_generation,object->source_generation,
            r->packet->input.generation,&r->state->palette,&checked);
        assert(bound==MH3_PACK_OK && !memcmp(&checked,&submission.mesh,sizeof checked));
#endif
        mh3_res_status s;
        submission.mesh.material=&r->state->material;submission.mesh.projection=r->packet->input.projection;
        submission.object=object;submission.member_index=r->current_member;submission.dispatch_flags=flags;
        submission.node=record->node;submission.material=record->material;submission.shape=record->shape;
        submission.translucent=(uint8_t)pass;s=r->emit(r->user,&submission);if(s) return s;
        ++r->stats.draws;previous=material_key;has_previous=1;
    }
    return MH3_RES_OK;
}
mh3_res_status mh3_owned_scene_render(mh3_owned_scene_packet *packet,mh3_owned_scene_work *work,
    mh3_owned_render_state *state,mh3_owned_scene_emit emit,void *user,mh3_owned_scene_stats *stats) {
    assert(packet && packet->admitted && work && state && emit);
    const uint32_t members=packet->input.member_count[0]+packet->input.member_count[1];
    assert(work->member_capacity>=members && work->bucket_capacity>=packet->bucket_total &&
        work->object_capacity>=packet->input.object_count && work->matrix_capacity>=packet->matrix_count &&
        work->sort_capacity>=MH3_MODEL_SORT_CAPACITY-1u);
    assert((!members || work->links) && work->buckets && (!packet->input.object_count || work->objects) &&
        (!packet->matrix_count || (work->world && work->position && work->normal && work->scale && work->attributes)) && work->sort);
    render r={0};r.packet=packet;r.work=work;r.state=state;r.emit=emit;r.user=user;r.stats.members=members;
    uint32_t first=0;
    for(uint32_t i=0;i<packet->input.object_count;i++) {
        work->objects[i].prepared=work->objects[i].viewed=0;
        work->objects[i].matrix_first=first;first+=packet->objects[i].model->matrix_count;
    }
    const mh3_scene_links links={&r,link_read,link_write,link_depth};first=0;
    for(unsigned category=0;category<6;category++) {
        r.heads[category]=UINT32_C(0x1000000)+first*4u;
        const int okay=mh3_scene_reset(&links,r.heads[category],packet->input.bucket_count[category]);assert(okay);(void)okay;
        first+=packet->input.bucket_count[category];
    }
    const mh3_scene_walk walk={&r,count,identity,read_node,phase,initialized,query,callback,prepare,apply_view,visibility,height,enqueue};
    mh3_scene_traverse(&walk,&packet->input.camera.v[0][0],packet->input.filter,packet->input.height_mode,
        packet->input.camera_height,packet->input.category_mode,packet->input.origin,packet->input.span);
    static const uint8_t passes[9][2]={{0,2},{5,0},{0,3},{2,2},{1,2},{4,0},{2,3},{1,3},{3,0}};
    mh3_res_status result=MH3_RES_OK;
    for(unsigned pass=0;pass<9;pass++) {
        uint32_t cursor=r.heads[passes[pass][0]],id;
        while((id=mh3_scene_next(&links,&cursor))) {
            r.current_member=node_index(&r,id);const mh3_owned_member *member=&packet->members[r.current_member];
            if(!mh3_scene_phase_selected(member->node.phase,passes[pass][1])) continue;
            r.current_object=member->object_index;
            result=draw_pass(&r,0);if(result) goto complete;
            result=draw_pass(&r,1);if(result) goto complete;
        }
    }
complete:
    if(stats) *stats=r.stats;return result;
}
