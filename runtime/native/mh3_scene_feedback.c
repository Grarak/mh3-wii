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
int mh3_scene_demo_setup(float w,float h,const mh3_draw_state *draw,const mh3_native_material_state *material,mh3_scene_quad_plan *out){
    if(!out||!draw||!material)return 0;mh3_scene_quad_plan q={0};if(!quad(&q,w,h,draw,material,1))return 0;
    q.raster.color_write=1;q.material.pixel.known|=(UINT64_C(1)<<MH3_PIXEL_ALPHA_COMPARE0)|(UINT64_C(1)<<MH3_PIXEL_ALPHA_REFERENCE0)|(UINT64_C(1)<<MH3_PIXEL_ALPHA_OPERATION)|(UINT64_C(1)<<MH3_PIXEL_ALPHA_COMPARE1)|(UINT64_C(1)<<MH3_PIXEL_ALPHA_REFERENCE1);
    q.material.pixel.values[MH3_PIXEL_ALPHA_COMPARE0]=q.material.pixel.values[MH3_PIXEL_ALPHA_COMPARE1]=7;q.material.pixel.values[MH3_PIXEL_ALPHA_REFERENCE0]=q.material.pixel.values[MH3_PIXEL_ALPHA_REFERENCE1]=0;q.material.pixel.values[MH3_PIXEL_ALPHA_OPERATION]=1;*out=q;return 1;
}
void mh3_scene_demo_quad(mh3_scene_quad_plan *q,int16_t x,int16_t y,int16_t w,int16_t h,const float uv0[2],const float uv1[2],const uint8_t rgba[4],int additive){
    const int16_t right=(int16_t)(x+w),bottom=(int16_t)(y+h);const float positions[4][3]={{x,y,0},{right,y,0},{right,bottom,0},{x,bottom,0}};const float uv[4][2]={{uv0[0],uv0[1]},{uv1[0],uv0[1]},{uv1[0],uv1[1]},{uv0[0],uv1[1]}};memcpy(q->vertices,positions,sizeof(positions));memcpy(q->uv,uv,sizeof(uv));memcpy(q->color,rgba,4);q->raster.blend_mode=1;q->raster.src_factor=4;q->raster.dst_factor=additive?1:5;q->raster.logic_op=0;
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

/* Both menus formerly sampled the rendered wheel. The shape callback samples
 * the authored sprite texture at the same selected CPU point instead. */
#define R32(a) o->read(o->context,(a),4)
#define R16(a) o->read(o->context,(a),2)
#define R8(a) o->read(o->context,(a),1)
#define S16(a) ((int16_t)R16(a))
#define W32(a,v) o->write(o->context,(a),(v),4)
#define W16(a,v) o->write(o->context,(a),(uint16_t)(v),2)
#define C(fn,...) cpu_call(o,(fn),(const uint32_t[]){__VA_ARGS__},sizeof((const uint32_t[]){__VA_ARGS__})/sizeof(uint32_t))
static void add_position(const mh3_scene_cpu_ops *o,uint32_t destination,uint32_t origin){W16(destination,S16(destination)+S16(origin));W16(destination+2,S16(destination+2)+S16(origin+2));}
int mh3_scene_inner_color_menu(const mh3_scene_cpu_ops *o,uint32_t self,uint32_t scratch){
    if(!o||!o->sample_shape||!o->begin_shape_sample)return 0;const uint32_t position=scratch+8,origin=scratch+12,offset=scratch+16,sprite=scratch+24;
    const uint32_t items=R8(self+2)?0x805b8f98:0x805b8fb4;
    C(0x802e05b4,0x1cf8,position);C(0x802e09a8,0x805b9290,position);C(0x802e09a8,R8(self+2)==1?0x807941d8:0x807941e0,position);
    C(0x802e05b4,0x1cf6,offset);C(0x802e05b4,0x1cf7,origin);C(0x802e0e70,0x805b925c,1,origin);C(0x802e0e70,0x805b9284,1,origin);
    const uint16_t selected=(uint16_t)C(0x801f3730,0x805b8f88,S16(self+4),S16(self+6)),count=(uint16_t)C(0x801f35ec,0x805b8f88);
    for(uint32_t i=0;(i&255)<count;i++){
        const uint32_t index=i&255;const int active=index==selected;uint32_t font=C(0x802aa450,1,active,1,0);const int16_t item=S16(items+index*2);
        if(item==-2){C(0x802e0b38,sprite,0x1cba,1,position);add_position(o,position,origin);C(0x802156c0,active,position);}
        else if(item==-3){
            if(active){C(0x802e0b38,sprite,0x1ca8,1,position);C(0x801e68b4,sprite,C(0x802e05b4,0x1ca6,0));W16(sprite,R16(self+0xe));W16(sprite+2,R16(self+0x10));add_position(o,position,origin);o->begin_shape_sample(o->context,(int16_t)(S16(position)+S16(sprite)),(int16_t)(S16(position+2)+S16(sprite+2)));}
            C(0x802e0e70,0x805b926c,active?(uint16_t)(S16(self+0xc)+10):1,origin);
            if(active){
                uint8_t rgba[4];if(!o->sample_shape(o->context,(int16_t)(S16(position)+S16(sprite)),(int16_t)(S16(position+2)+S16(sprite+2)),rgba))return 0;W32(self+0x14,((uint32_t)rgba[0]<<24)|((uint32_t)rgba[1]<<16)|((uint32_t)rgba[2]<<8)|255);C(0x802e07a4,sprite,position);
            }
        }else{C(0x802e05b4,R16(0x805b9242+index*2),position);add_position(o,position,offset);C(0x801f9b5c,position,font,active,(uint32_t)(int32_t)item,1);}
    }
    C(0x801e68b4,sprite,C(0x802e05b4,0x1cb2,0));W32(sprite+0x1c,R32(self+0x18));C(0x802e0e0c,sprite,1,origin);
    int32_t help=-1;if(R8(self)==0){C(0x801f5528);if(R8(self+2)==0)help=0x11c;else if(R8(self+2)==1)help=0x11f;}else if(R8(self)==1){help=0x124;C(0x802151d4,0x1879,R32(self+0x20));}if(help!=-1)C(0x80214f54,0x1877,(uint32_t)help);return 1;
}
int mh3_scene_equipment_color_menu(const mh3_scene_cpu_ops *o,uint32_t self,uint32_t scratch){
    if(!o||!o->sample_shape||!o->begin_shape_sample)return 0;const uint32_t position=scratch+8,origin=scratch+12,offset=scratch+16,sprite=scratch+24,temporary=scratch+64;
    if(R8(self)-1u<2){
        const int alternate=R8(self+3)!=1;const uint32_t items=alternate?0x805b8f68:0x805b8f38;const int16_t selected=(int16_t)C(0x801f3730,alternate?0x805b8f58:0x805b8f28,S16(self+6),S16(self+8));
        C(0x802e0b38,sprite,0x1cbb,alternate,position);C(0x802e0e70,0x805b9208,alternate,position);C(0x802e0e20,R16(0x805b9230+S16(self+4)*2),alternate,position);
        C(0x802e0b38,sprite,0x1c97,alternate,offset);C(0x802e0b38,sprite,0x1ca5,alternate,origin);C(0x802e0e70,0x805b925c,alternate,origin);C(0x802e0e70,0x805b9284,alternate,origin);
        const uint16_t count=(uint16_t)C(0x801f3f40,self);
        for(uint32_t i=0;(i&255)<count;i++){
            const uint32_t index=i&255;const int active=(int32_t)index==selected;const uint32_t font=C(0x802aa450,1,active,1,0);const int16_t item=S16(items+index*2);
            if(item==-1){C(0x802e05b4,R16(0x805b9240+index*2),position);add_position(o,position,offset);C(0x80215cfc,C(0x80211e80,0,0x231),active,position,font,9);}
            else if(item==-2){C(0x802e0b38,sprite,0x1cba,alternate,position);add_position(o,position,origin);C(0x802156c0,active,position);}
            else if(item==-3){
                if(active){C(0x802e0b38,sprite,0x1ca8,alternate,position);C(0x801e68b4,sprite,C(0x802e05b4,0x1ca6,0));W16(sprite,R16(self+0x10));W16(sprite+2,R16(self+0x12));add_position(o,position,origin);o->begin_shape_sample(o->context,(int16_t)(S16(position)+S16(sprite)),(int16_t)(S16(position+2)+S16(sprite+2)));}
                C(0x802e0e70,0x805b926c,active?(uint16_t)(S16(self+0xe)+10):1,origin);
                if(active){
                    uint8_t rgba[4];if(!o->sample_shape(o->context,(int16_t)(S16(position)+S16(sprite)),(int16_t)(S16(position+2)+S16(sprite+2)),rgba))return 0;W16(self+0x14,((rgba[0]&248)<<8)|((rgba[1]&252)<<3)|(rgba[2]>>3));C(0x802e07a4,sprite,position);
                }
            }else{C(0x802e05b4,R16(0x805b9240+index*2),position);add_position(o,position,offset);C(0x801f9b5c,position,font,active,(uint32_t)(int32_t)item,1);}
        }
        if(S16(self+4)!=0||R8(self+0x22)!=1){C(0x801e68b4,sprite,C(0x802e05b4,0x1cb2,0));const uint16_t packed=(uint16_t)R16(self+0x20);W32(sprite+0x1c,((uint32_t)(packed&0xf800)<<16)|((uint32_t)(packed&0x7e0)<<13)|((uint32_t)(packed&31)<<11)|255);C(0x802e0e0c,sprite,alternate,origin);}
        int32_t help=-1;if(R8(self)==1){C(0x801f5528);help=0x123;}else if(R8(self)==2){help=0x124;C(0x802151d4,0x1879,R32(self+0x24));}if(help!=-1)C(0x80214f54,0x1877,(uint32_t)help);
    }else if(R8(self)==0){
        C(0x802e05b4,0x1c7d,position);C(0x802e09a8,0x805b91d0,position);C(0x802e05b4,0x1c76,offset);
        for(uint32_t i=0;i<6;i++){
            const int active=i==(uint32_t)(int32_t)S16(self+4);const int enabled=C(0x801f4280,R32(self+0x28),i)==1;const uint32_t font=C(0x802aa450,enabled,active,1,0);
            if(!i){C(0x802e05b4,R16(0x805b91f4),position);add_position(o,position,offset);C(0x80215cfc,C(0x80211e80,0,0x232),active,position,font,9);}
            else{const uint32_t owner=R32(self+0x28),record=owner+0x140+R8(0x80794164+i)*12;uint32_t locked=0;if(S16(record+2)!=0&&C(0x8027e854,record)!=1)locked=R8(owner+0x262);W32(temporary,R32(offset));C(0x8030c890,record,0,active,!enabled,locked,temporary,R16(0x805b91f4+i*2));}
        }
        C(0x801f5528);C(0x80214f54,0x1877,0x121);
    }
    return 1;
}
#undef R32
#undef R16
#undef R8
#undef S16
#undef W32
#undef W16
#undef C

static uint16_t source_u16(const uint8_t *p){return (uint16_t)((uint16_t)p[0]<<8|p[1]);}
static uint8_t expand5(uint32_t v){return (uint8_t)(v<<3|v>>2);}
static uint8_t expand6(uint32_t v){return (uint8_t)(v<<2|v>>4);}
static void source_color16(uint16_t value,uint32_t format,uint8_t rgba[4]){
    if(format==0){rgba[0]=rgba[1]=rgba[2]=(uint8_t)value;rgba[3]=(uint8_t)(value>>8);}
    else if(format==1){rgba[0]=expand5(value>>11);rgba[1]=expand6(value>>5&63);rgba[2]=expand5(value&31);rgba[3]=255;}
    else if(value&0x8000){rgba[0]=expand5(value>>10&31);rgba[1]=expand5(value>>5&31);rgba[2]=expand5(value&31);rgba[3]=255;}
    else{rgba[0]=(uint8_t)((value>>8&15)*17);rgba[1]=(uint8_t)((value>>4&15)*17);rgba[2]=(uint8_t)((value&15)*17);const uint32_t a=value>>12&7;rgba[3]=(uint8_t)(a<<5|a<<2|a>>1);}
}
static int source_texel(mh3_res_span pixels,mh3_res_span palette,uint32_t palette_format,const mh3_native_texture_binding *b,uint32_t x,uint32_t y,float out[4]){
    const uint32_t f=b->source.format,w=b->source.width;uint32_t bw=4,bh=4,bytes=32;
    if(f==0||f==8||f==14){bw=bh=8;}else if(f==1||f==2||f==9){bw=8;bh=4;}else if(f==6)bytes=64;else if(f!=3&&f!=4&&f!=5&&f!=10)return 0;
    const size_t tile=((size_t)(y/bh)*((w+bw-1)/bw)+x/bw)*bytes;
    if(!pixels.data||tile>pixels.size||bytes>pixels.size-tile)return 0;
    const uint8_t *p=pixels.data+tile;const uint32_t i=(y%bh)*bw+x%bw;uint8_t rgba[4]={0,0,0,255};uint32_t index=0;
    switch(f){
    case 0:rgba[0]=rgba[1]=rgba[2]=(uint8_t)((p[i/2]>>((i&1)?0:4)&15)*17);rgba[3]=rgba[0];break;
    case 1:rgba[0]=rgba[1]=rgba[2]=p[i];rgba[3]=rgba[0];break;
    case 2:rgba[0]=rgba[1]=rgba[2]=(uint8_t)((p[i]&15)*17);rgba[3]=(uint8_t)((p[i]>>4)*17);break;
    case 3:rgba[0]=rgba[1]=rgba[2]=p[2*i+1];rgba[3]=p[2*i];break;
    case 4:source_color16(source_u16(p+2*i),1,rgba);break;
    case 5:source_color16(source_u16(p+2*i),2,rgba);break;
    case 6:rgba[0]=p[2*i+1];rgba[1]=p[32+2*i];rgba[2]=p[33+2*i];rgba[3]=p[2*i];break;
    case 8:index=p[i/2]>>((i&1)?0:4)&15;break;
    case 9:index=p[i];break;
    case 10:index=source_u16(p+2*i)&0x3fff;break;
    case 14:{
        p+=((y%8)/4*2+(x%8)/4)*8;const uint16_t a=source_u16(p),c=source_u16(p+2);const uint32_t code=p[4+y%4]>>(6-2*(x%4))&3;uint8_t first[4],second[4];source_color16(a,1,first);source_color16(c,1,second);
        for(unsigned k=0;k<3;k++)rgba[k]=code==0?first[k]:code==1?second[k]:a>c?(uint8_t)(((code==2?5:3)*first[k]+(code==2?3:5)*second[k])/8):(uint8_t)((first[k]+second[k])/2);rgba[3]=a<=c&&code==3?0:255;break;
    }}
    if(f==8||f==9||f==10){if(palette_format>2||!palette.data||(size_t)index*2+2>palette.size)return 0;source_color16(source_u16(palette.data+index*2),palette_format,rgba);}
    for(unsigned k=0;k<4;k++)out[k]=(float)rgba[k]/255;return 1;
}
static uint32_t source_wrap(int32_t value,uint32_t size,uint32_t mode){
    if(!mode)return value<0?0:(uint32_t)value>=size?size-1:(uint32_t)value;
    int32_t period=(int32_t)(mode==2?size*2:size),wrapped=value%period;if(wrapped<0)wrapped+=period;if(mode==2&&(uint32_t)wrapped>=size)wrapped=period-1-wrapped;return (uint32_t)wrapped;
}
int mh3_scene_source_quad(mh3_res_span pixels,mh3_res_span palette,uint32_t palette_format,const mh3_native_texture_binding *b,const AuroraNativeVertex vertices[4],const mh3_draw_state *s,const float point[2],float out[4]){
    if(!b||!vertices||!s||!point||!out||!b->source.width||!b->source.height||b->source.width>65535||b->source.height>65535||b->sampler.wrap_s>2||b->sampler.wrap_t>2||!isfinite(point[0])||!isfinite(point[1]))return -1;
    float x[4],y[4];for(unsigned i=0;i<4;i++){const float w=vertices[i].position[3];if(!isfinite(w)||w==0)return -1;x[i]=s->viewport[0]+(vertices[i].position[0]/w+1)*s->viewport[2]*.5f;y[i]=s->viewport[1]+(1-vertices[i].position[1]/w)*s->viewport[3]*.5f;}
    const unsigned triangles[2][3]={{0,1,2},{0,2,3}};float uv[2]={0},color[4]={0};int covered=0;
    for(unsigned t=0;t<2&&!covered;t++){
        const unsigned a=triangles[t][0],c=triangles[t][1],d=triangles[t][2];const float det=(y[c]-y[d])*(x[a]-x[d])+(x[d]-x[c])*(y[a]-y[d]);if(det==0)continue;
        const float wa=((y[c]-y[d])*(point[0]-x[d])+(x[d]-x[c])*(point[1]-y[d]))/det,wc=((y[d]-y[a])*(point[0]-x[d])+(x[a]-x[d])*(point[1]-y[d]))/det,wd=1-wa-wc;
        if(wa<0||wc<0||wd<0)continue;const float weights[3]={wa/vertices[a].position[3],wc/vertices[c].position[3],wd/vertices[d].position[3]},sum=weights[0]+weights[1]+weights[2];if(sum==0)return -1;
        for(unsigned v=0;v<3;v++){const unsigned index=triangles[t][v];for(unsigned k=0;k<2;k++)uv[k]+=vertices[index].uv[k]*weights[v]/sum;for(unsigned k=0;k<4;k++)color[k]+=(float)vertices[index].color[k]/255*weights[v]/sum;}covered=1;
    }
    if(!covered)return 0;const float fx=uv[0]*b->source.width-.5f,fy=uv[1]*b->source.height-.5f;if(!isfinite(fx)||!isfinite(fy)||fabsf(fx)>1e8f||fabsf(fy)>1e8f)return -1;
    if(b->sampler.mag_filter){const int32_t ix=(int32_t)floorf(fx),iy=(int32_t)floorf(fy);const float dx=fx-ix,dy=fy-iy;float result[4]={0};for(unsigned v=0;v<4;v++){float texel[4];const uint32_t px=source_wrap(ix+(v&1),b->source.width,b->sampler.wrap_s),py=source_wrap(iy+(v>>1),b->source.height,b->sampler.wrap_t);if(!source_texel(pixels,palette,palette_format,b,px,py,texel))return -1;const float weight=(v&1?dx:1-dx)*(v&2?dy:1-dy);for(unsigned k=0;k<4;k++)result[k]+=texel[k]*weight;}for(unsigned k=0;k<4;k++)out[k]=result[k]*color[k];}
    else{float texel[4];if(!source_texel(pixels,palette,palette_format,b,source_wrap((int32_t)floorf(fx+.5f),b->source.width,b->sampler.wrap_s),source_wrap((int32_t)floorf(fy+.5f),b->source.height,b->sampler.wrap_t),texel))return -1;for(unsigned k=0;k<4;k++)out[k]=texel[k]*color[k];}
    return 1;
}
