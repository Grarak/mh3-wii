#include "mh3_scene_feedback.h"
#include <math.h>
#include <string.h>

static void stage(mh3_pack_tev_stage *s,int modulate){
    memset(s,0,sizeof(*s));s->known=(UINT64_C(1)<<MH3_TEV_FIELD_COUNT)-1;
    s->texture_coordinate=s->texture_map=0;s->raster_channel=modulate?4:255;
    s->color=(mh3_pack_tev_pass){15,modulate?8:15,modulate?10:15,modulate?15:8,0,0,0,1,0};
    s->alpha=(mh3_pack_tev_pass){7,modulate?4:7,modulate?5:7,modulate?7:1,0,0,0,1,0};
}
static void material_quad(mh3_native_material_state *m,int modulate){
    m->texgen_count=1;m->tev_count=1;m->channel_count=modulate?1:0;stage(&m->tev[0],modulate);
    m->texgen[0].known=(1u<<MH3_TEXGEN_FIELD_COUNT)-1;memset(m->texgen[0].values,0,sizeof(m->texgen[0].values));
    m->texgen[0].values[MH3_TEXGEN_SOURCE_ROW]=5;m->texgen[0].values[MH3_TEXGEN_MATRIX_ROW]=60;m->texgen[0].values[MH3_TEXGEN_POST_MATRIX_ROW]=61;
    if(modulate){m->channel[0].color=(mh3_pack_channel_control){1,0,0,0,2,0};m->channel[0].alpha=m->channel[0].color;m->channel[0].flags|=48;}
    m->swaps[0]=(mh3_pack_swap_table){15,{0,1,2,3}};
}
static int quad(mh3_scene_quad_plan *q,float w,float h,const mh3_draw_state *draw,
                 const mh3_native_material_state *material,int modulate){
    if(!isfinite(w)||!isfinite(h)||w<=0||h<=0||w>32767||h>32767)return 0;
    if(!mh3_draw_ortho(q->projection,0,h,0,w,0,1))return 0;
    mh3_draw_identity_position(q->position);q->raster=draw->raster;q->material=*material;material_quad(&q->material,modulate);
    q->raster.cull_mode=0;q->raster.depth_test=1;q->raster.depth_compare=7;q->raster.depth_write=0;
    const float positions[4][3]={{0,0,0},{(float)(int16_t)(int32_t)w,0,0},{(float)(int16_t)(int32_t)w,(float)(int16_t)(int32_t)h,0},{0,(float)(int16_t)(int32_t)h,0}};
    const float uv[4][2]={{0,0},{1,0},{1,1},{0,1}};memcpy(q->vertices,positions,sizeof(positions));memcpy(q->uv,uv,sizeof(uv));memset(q->color,255,4);return 1;
}
int mh3_scene_capture_quad(uint32_t id,float tw,float th,float sw,float sh,uint8_t tint,int subtract,
                           const mh3_draw_state *draw,const mh3_native_material_state *material,mh3_scene_quad_plan *out){
    if(!out||!draw||!material||!id||!isfinite(tw)||!isfinite(th)||tw<1||th<1||tw>65535||th>65535)return 0;
    mh3_scene_quad_plan q={0};if(!quad(&q,sw,sh,draw,material,1))return 0;
    q.identity=id;q.width=(uint16_t)(int32_t)tw;q.height=(uint16_t)(int32_t)th;q.format=6;
    q.color[0]=q.color[1]=q.color[2]=tint;q.raster.blend_mode=subtract?3:1;q.raster.src_factor=subtract?2:1;q.raster.dst_factor=subtract?2:0;q.raster.logic_op=0;*out=q;return 1;
}
int mh3_scene_temporal_prepare(const mh3_temporal_feedback_input *in,const mh3_draw_state *draw,
                               const mh3_native_material_state *material,mh3_temporal_feedback_plan *out){
    if(!in||!draw||!material||!out)return 0;mh3_temporal_feedback_plan p={0};p.frames=in->frames;p.enabled=in->enabled;
    if(!p.enabled){*out=p;return 1;}if(!in->identity||in->screen_width<2||in->screen_height<2)return 0;
    p.frames++;p.capture=(mh3_scene_capture_plan){in->identity,in->screen_width/2,in->screen_height/2,4,{0,0,in->screen_width,in->screen_height},1};
    if(!quad(&p.previous,(float)in->screen_width,in->projection_height,draw,material,0))return 0;
    p.previous.identity=p.capture.identity;p.previous.width=p.capture.width;p.previous.height=p.capture.height;p.previous.format=4;
    p.previous.raster.depth_test=0;p.previous.raster.color_write=1;p.previous.raster.blend_mode=in->mode==2?3:1;p.previous.raster.src_factor=4;p.previous.raster.dst_factor=in->mode==1?1:5;p.previous.raster.logic_op=0;
    p.previous.material.color[1].known=15;for(unsigned i=0;i<3;i++)p.previous.material.color[1].rgba[i]=in->mode==1?in->amount:in->tint[i];p.previous.material.color[1].rgba[3]=in->alpha;
    if(in->mode==1){p.previous.material.tev[0].color=(mh3_pack_tev_pass){2,15,15,8,1,0,in->subtract_scale,1,0};}
    /* DOL807980DC is -1; temporal feedback differs from scene draw-back. */
    if(!mh3_draw_ortho(p.previous.projection,0,in->projection_height,0,(float)in->screen_width,0,-1))return 0;
    p.initial=p.previous;p.initial.material=*material;p.initial.material.color[1].known=15;
    for(unsigned i=0;i<3;i++)p.initial.material.color[1].rgba[i]=in->tint[i];p.initial.material.color[1].rgba[3]=in->alpha;
    p.initial.raster.blend_mode=draw->raster.blend_mode;p.initial.raster.src_factor=draw->raster.src_factor;p.initial.raster.dst_factor=draw->raster.dst_factor;p.initial.raster.logic_op=draw->raster.logic_op;
    /* DOL807980E0 is 1/1024, not a normalized byte factor. */
    if(in->mode==3){float f=(float)in->amount/1024.0f,s0=f+in->offset[0]*f,s1=(1-f)+in->offset[0]*f,t0=f+in->offset[1]*f,t1=(1-f)+in->offset[1]*f;const float uv[4][2]={{s0,t0},{s1,t0},{s1,t1},{s0,t1}};memcpy(p.previous.uv,uv,sizeof(uv));}
    p.draw_previous=p.frames!=1&&in->alpha!=0;
    p.threshold=p.previous;p.threshold.material.channel_count=1;p.threshold.raster.alpha_write=0;
    p.threshold.raster.blend_mode=1;p.threshold.raster.dst_factor=1;
    p.threshold.material.color[1]=(mh3_pack_tev_color){15,{in->threshold_gray,in->threshold_gray,in->threshold_gray,in->threshold_alpha}};
    p.threshold.material.tev[0].color=(mh3_pack_tev_pass){15,2,12,8,1,0,(uint8_t)(in->threshold_mode?in->threshold_mode-1:0),1,0};
    const float full[4][2]={{0,0},{1,0},{1,1},{0,1}};memcpy(p.threshold.uv,full,sizeof(full));p.draw_threshold=in->threshold_mode!=0;*out=p;return 1;
}
int mh3_scene_glare_prepare(const mh3_scene_glare_input *in,mh3_scene_glare_plan *out){
    if(!in||!out||!in->identity||in->levels<0||in->levels>MH3_SCENE_GLARE_LEVELS||in->start_width<=0||in->start_height<=0||!isfinite(in->darken)||!isfinite(in->brighten)||!isfinite(in->projection_width)||!isfinite(in->projection_height)||in->projection_width<=0||in->projection_height<=0)return 0;
    mh3_scene_glare_plan p={0};p.identity=in->identity;p.level_count=(uint32_t)in->levels;p.projection_width=in->projection_width;p.projection_height=in->projection_height;
    p.low_threshold=(float)in->low_threshold/255;p.high_threshold=(float)in->high_threshold/255;p.darken=in->darken;p.brighten=in->brighten;p.shift_scale=ldexpf(1,-(int)(in->shift&31));
    uint32_t sw=(uint16_t)(in->start_width<<1),sh=(uint16_t)(in->start_height<<1),w=(uint16_t)in->start_width,h=(uint16_t)in->start_height;
    for(unsigned i=0;i<p.level_count;i++){
        if(!w||!h)return 0;p.levels[i]=(mh3_scene_glare_level){(uint16_t)sw,(uint16_t)sh,(uint16_t)w,(uint16_t)h,(int16_t)w,(int16_t)h};sw>>=1;sh>>=1;w>>=1;h>>=1;
    }
    p.width=(uint16_t)sw;p.height=(uint16_t)sh;*out=p;return 1;
}
float mh3_scene_glare_brightness(const mh3_scene_glare_plan *p,const float rgb[3]){
    float l=(77*rgb[0]+150*rgb[1]+29*rgb[2])/256;
    if(l<p->low_threshold)l-=l*p->darken;if(l>p->high_threshold)l+=l*p->brighten;
    return fminf(fmaxf(l,0),1)*p->shift_scale;
}

static uint32_t cpu_call(const mh3_scene_cpu_ops *o,uint32_t fn,const uint32_t *args,unsigned count){return o->call(o->context,fn,args,count);}
#define RC(a) o->read(o->context,(a),4)
#define RH(a) o->read(o->context,(a),2)
#define RB(a) o->read(o->context,(a),1)
#define WC(a,v) o->write(o->context,(a),(v),4)
#define WB(a,v) o->write(o->context,(a),(v),1)
#define CALL(fn,...) cpu_call(o,(fn),(const uint32_t[]){__VA_ARGS__},sizeof((const uint32_t[]){__VA_ARGS__})/sizeof(uint32_t))
int mh3_scene_equipment_update(const mh3_scene_cpu_ops *o,uint32_t owner,uint32_t work,uint32_t input_slot,uint32_t scratch){
    const uint32_t slot=input_slot&255,offset=slot*4;
    if(!o||!o->read||!o->write||!o->call||!o->copy||!o->zero||!o->copy_string||!o->compare_string||!o->texture_object||slot>=10)return 0;
    const uint32_t model=scratch+24,material=scratch+20,texture=scratch+16,name=scratch+32;
    CALL(0x8007b8dc,model,0);CALL(0x80078a94,material,0);CALL(0x8006f520,texture,0);
    WC(work+offset+0x120,0);const uint32_t equipment=RC(owner+0x13c),record=equipment+(slot<7?slot*0x164+4:(slot-7)*0x164+0xa14);
    if(RC(work+offset+0x9c)!=UINT32_MAX){
        CALL(0x80268750,work,record,slot);WC(work+offset+200,UINT32_MAX);WC(work+offset+0xf4,UINT32_MAX);
        if(slot==7){if(RC(work+0xbc)!=UINT32_MAX){CALL(0x80268750,work,equipment+0xb78,8);WC(work+0xe8,UINT32_MAX);WC(work+0x114,UINT32_MAX);}if(RC(work+0xc0)!=UINT32_MAX){CALL(0x80268750,work,equipment+0xcdc,9);WC(work+0xec,UINT32_MAX);WC(work+0x118,UINT32_MAX);}}
    }
    if(RC(work+offset+0x18)==UINT32_MAX){
        WC(work+offset+0x9c,UINT32_MAX);WC(work+offset+200,UINT32_MAX);WC(work+offset+0xf4,UINT32_MAX);
        if(slot<7){CALL(0x8022315c,equipment);WB(equipment+slot+0x9c0,0);}else{CALL(0x80223210,equipment,slot-7);WB(equipment+slot-7+0x9c7,0);}
    }else{
        WC(work+offset+200,RC(RC(0x80796fe8)+RB(owner+8)*0x2c+offset+0x1c));WC(work+offset+0x9c,RC(work+offset+0x18));
        if(slot<7){
            const uint32_t source=CALL(0x800d5168,RC(work+offset+0x44)),destination=CALL(0x800d5168,RC(work+offset+200));
            if(destination!=source){const uint32_t bytes=(uint32_t)((int32_t)(int16_t)RH(0x805c6618+slot*2)*0x2800);o->copy(o->context,destination,source,bytes);o->zero(o->context,source,bytes);}
            o->copy_string(o->context,name,CALL(0x80268550,RB(owner+0x14),slot,RC(work+offset+0x18)));WC(work+offset+0xf4,CALL(0x800d5758,name,destination));CALL(0x80223d50,equipment,slot);
            if(RC(record+0x10c))WB(RC(record+0x10c)+3,4);WC(work+offset+0x120,equipment+slot+0x9c0);
            if(slot==4){if(RC(work+0xb4)!=UINT32_MAX)CALL(0x80223c30,equipment);if(RC(work+0xb0)!=UINT32_MAX)CALL(0x80223b40,equipment);}
        }else{
            uint32_t kind=RC(work+offset+0x18)>>24;if(kind-4<3){if(slot!=7)kind=slot+3;}else if(kind==1){if(slot!=7)kind=9;}else if(kind==3&&slot!=7)kind=10;
            const uint32_t source_base=CALL(0x800d5168,RC(work+offset+0x44)),dest_base=CALL(0x800d5168,RC(work+offset+200));const uint32_t delta=(uint32_t)((int32_t)(int16_t)RH(0x805c6644+kind*2)*0x5000),source=source_base+delta,destination=dest_base+delta;
            if(destination!=source)o->copy(o->context,destination,source,(uint32_t)((int32_t)(int16_t)RH(0x805c6628+kind*2)*0x5000));
            o->copy_string(o->context,name,CALL(0x80268668,slot,RC(work+offset+0x18)));WC(work+offset+0xf4,CALL(0x800d5758,name,destination));CALL(0x80223f54,equipment,(slot-7)&255);
            if(RC(record+0x10c))WB(RC(record+0x10c)+3,4);WC(work+offset+0x120,equipment+slot-7+0x9c7);
        }
    }
    if((slot==0||slot>6)&&RB(work+slot+0xb))CALL(0x8022431c,equipment);
    if(slot-4<2)CALL(0x802abf64,owner);
    WB(work+slot+0xb,0);WC(work+offset+0x18,UINT32_MAX);WC(work+offset+0x44,UINT32_MAX);WC(work+offset+0x70,UINT32_MAX);WB(work+slot,8);WC(RC(0x80796fe8)+offset+0x200,0);
    if(slot<7&&slot!=5&&RC(work+offset+0x9c)!=UINT32_MAX){
        const uint32_t payload=CALL(0x800474d4,RB(owner+8),slot==6);
        if(payload){
            CALL(0x8007ba6c,model,equipment+slot*0x164+0x118);const uint32_t count=CALL(0x800988cc,model);
            for(uint32_t i=0;i<count;i++){
                WC(scratch+12,CALL(0x80098864,model,i));CALL(0x8007892c,material,scratch+12);const uint32_t material_name=CALL(0x80223608,material);
                if(material_name&&o->compare_string(o->context,material_name,slot==6?0x8079454c:0x807945f4)==0){
                    WC(scratch+8,CALL(0x8006f4ec,material));CALL(0x800786b4,texture,scratch+8);
                    for(uint32_t map=0;map<8;map++){const uint32_t object=CALL(0x800948d4,texture,map);if(object)o->texture_object(o->context,object,payload);}
                }
            }
        }
    }
    return 1;
}
#undef RC
#undef RH
#undef RB
#undef WC
#undef WB
#undef CALL
