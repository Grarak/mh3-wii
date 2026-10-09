#include "mh3_effect_draw.h"
#include "mh3_madd.h"
#include <math.h>
#include <string.h>

static uint16_t be16(const uint8_t *p){return (uint16_t)((uint16_t)p[0]<<8|p[1]);}
static uint32_t be32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static float bef(const uint8_t *p){uint32_t n=be32(p);float f;memcpy(&f,&n,4);return f;}
static uint8_t product(uint8_t a,uint8_t b){return (uint8_t)(((uint32_t)a*b+128)>>8);}
static void pixel(mh3_pack_pixel_inputs *p,uint32_t field,uint32_t value){p->values[field]=value;p->known|=UINT64_C(1)<<field;}

mh3_res_status mh3_effect_material_read(mh3_res_span resource,mh3_effect_material *out){
    if(!out||!resource.data||resource.size<0xac)return MH3_RES_RANGE;
    const uint8_t *r=resource.data;mh3_effect_material m;memset(&m,0,sizeof(m));
    m.flags=be16(r);m.stage_count=r[5];m.indirect_mask=r[7];
    if(m.stage_count>4)return MH3_RES_INVALID;
    for(uint32_t i=0;i<3;i++)m.texture_maps[i]=-1;
    for(uint32_t i=0;i<3;i++)if(m.flags&(0x10u<<i))m.texture_maps[i]=(int8_t)m.texture_count++;
    for(uint32_t i=0;i<m.stage_count;i++){
        mh3_pack_tev_stage *s=&m.stages[i];
        const uint8_t *c=r+0xc+4*i,*a=r+0x30+4*i,*co=r+0x1c+5*i,*ao=r+0x40+5*i;
        s->color=(mh3_pack_tev_pass){c[0],c[1],c[2],c[3],co[0],co[1],co[2],co[3],co[4]};
        s->alpha=(mh3_pack_tev_pass){a[0],a[1],a[2],a[3],ao[0],ao[1],ao[2],ao[3],ao[4]};
        if(c[0]>15||c[1]>15||c[2]>15||c[3]>15||a[0]>7||a[1]>7||a[2]>7||a[3]>7)return MH3_RES_INVALID;
        if(co[1]>2||co[2]>3||co[3]>1||co[4]>3||ao[1]>2||ao[2]>3||ao[3]>1||ao[4]>3)return MH3_RES_INVALID;
        s->texture_map=s->texture_coordinate=r[8+i]<=1?(uint8_t)m.texture_maps[r[8+i]]:255;
        if(r[8+i]<=1&&m.texture_maps[r[8+i]]<0)return MH3_RES_INVALID;
        s->raster_channel=4;s->konst_color=r[0x54+i];s->konst_alpha=r[0x58+i];
        s->known=(UINT64_C(1)<<MH3_TEV_FIELD_COUNT)-1;
        if((m.flags&0x40)&&(m.indirect_mask&(1u<<i))){s->indirect_bias=7;s->indirect_matrix=1;}
    }
    for(uint32_t i=0;i<6;i++){m.indirect_matrix[i]=bef(r+0x90+4*i);if((m.flags&0x40)&&(!isfinite(m.indirect_matrix[i])||m.indirect_matrix[i]<-32||m.indirect_matrix[i]>=32))return MH3_RES_INVALID;}
    m.indirect_exponent=(int8_t)r[0xa8];
    memcpy(m.rgb_sources,r+0x60,8);memcpy(m.alpha_sources,r+0x68,8);
    for(uint32_t i=0;i<8;i++)if(m.rgb_sources[i]>6||m.alpha_sources[i]>6)return MH3_RES_INVALID;
    pixel(&m.pixel,MH3_PIXEL_ALPHA_COMPARE0,r[2]);pixel(&m.pixel,MH3_PIXEL_ALPHA_COMPARE1,r[3]);
    pixel(&m.pixel,MH3_PIXEL_ALPHA_OPERATION,r[4]);
    pixel(&m.pixel,MH3_PIXEL_DEPTH_TEST,(m.flags&1)!=0);pixel(&m.pixel,MH3_PIXEL_DEPTH_WRITE,(m.flags&2)!=0);
    pixel(&m.pixel,MH3_PIXEL_DEPTH_COMPARE,r[0x70]);
    pixel(&m.pixel,MH3_PIXEL_BLEND_ENABLE,r[0x5c]==1);pixel(&m.pixel,MH3_PIXEL_LOGIC_ENABLE,r[0x5c]==2);
    pixel(&m.pixel,MH3_PIXEL_BLEND_SUBTRACT,r[0x5c]==3);pixel(&m.pixel,MH3_PIXEL_BLEND_SOURCE,r[0x5d]);
    pixel(&m.pixel,MH3_PIXEL_BLEND_DESTINATION,r[0x5e]);pixel(&m.pixel,MH3_PIXEL_LOGIC_OPERATION,r[0x5f]);
    pixel(&m.pixel,MH3_PIXEL_DEPTH_TEXTURE_OPERATION,0);pixel(&m.pixel,MH3_PIXEL_DITHER,0);
    *out=m;return MH3_RES_OK;
}
void mh3_effect_material_apply(const mh3_effect_material *r,const mh3_native_material_state *inherited,mh3_native_material_state *out){
    mh3_native_material_state m=*inherited;m.texgen_count=r->texture_count;m.tev_count=r->stage_count;m.cull_mode=0;m.clip_mode_known=1;m.clip_disabled=(r->flags&8)!=0;m.indirect_count=(r->flags&0x40)?1:0;
    memcpy(m.tev,r->stages,r->stage_count*sizeof(r->stages[0]));
    m.swaps[0]=(mh3_pack_swap_table){15,{0,1,2,3}};
    for(uint32_t f=0;f<MH3_PIXEL_FIELD_COUNT;f++)if(r->pixel.known&(UINT64_C(1)<<f)){m.pixel.known|=UINT64_C(1)<<f;m.pixel.values[f]=r->pixel.values[f];}
    if(m.indirect_count){m.indirect[0]=(mh3_pack_indirect_stage){15,(uint8_t)r->texture_maps[2],(uint8_t)r->texture_maps[2],0,0};m.indirect_matrix[0].known=127;m.indirect_matrix[0].scale_exponent=r->indirect_exponent;for(uint32_t k=0;k<6;k++){int32_t coefficient=(int32_t)(r->indirect_matrix[k]*1024.0f);coefficient&=2047;if(coefficient>=1024)coefficient-=2048;m.indirect_matrix[0].coefficients[k]=(int16_t)coefficient;}}
    *out=m;
}
void mh3_effect_color(uint8_t source,const uint8_t colors[4][4],uint8_t out[3]){
    for(uint32_t k=0;k<3;k++)out[k]=source>=1&&source<=4?colors[source-1][k]:source==5?product(colors[0][k],colors[1][k]):source==6?product(colors[2][k],colors[3][k]):0;
}
uint8_t mh3_effect_alpha(uint8_t source,const uint8_t colors[4][4]){
    return source>=1&&source<=4?colors[source-1][3]:source==5?product(colors[0][3],colors[1][3]):source==6?product(colors[2][3],colors[3][3]):0;
}
void mh3_effect_register_colors(const mh3_effect_material *m,const uint8_t colors[4][4],uint8_t tev[3][4],uint8_t konst[4][4]){
    for(uint32_t i=0;i<7;i++){
        uint32_t slot=i+1; /* sources1..7: TEV1..3 then K0..3 */
        uint8_t *out=i<3?tev[i]:konst[i-3];
        mh3_effect_color(m->rgb_sources[slot],colors,out);out[3]=mh3_effect_alpha(m->alpha_sources[slot],colors);
    }
}
mh3_res_status mh3_effect_texture_read(mh3_res_span r,uint32_t address,uint32_t map,uint32_t ws,uint32_t wt,mh3_native_texture_binding *out,mh3_native_palette_source *palette){
    if(!out||!palette||!r.data||r.size<0x24)return MH3_RES_RANGE;
    mh3_native_texture_binding b;mh3_native_palette_source p;memset(&b,0,sizeof(b));memset(&p,0,sizeof(p));
    b.source.resource_address=address;b.source.width=be16(r.data+4);b.source.height=be16(r.data+6);b.source.format=r.data[0xc];
    /* 800C7D44 enables mipmaps only when the authored count is >1. A count
     * of zero is a valid base-only resource; its requested max LOD (-1) was
     * clamped to zero by the original sampler initializer. */
    b.source.levels=r.data[0x14]?r.data[0x14]:1u;
    b.source.max_lod=fminf((float)b.source.levels-1.0f,10.0f);b.source.payload_address=be32(r.data+0x1c);
    b.sampler.wrap_s=ws;b.sampler.wrap_t=wt;b.sampler.min_filter=r.data[0x15];b.sampler.mag_filter=r.data[0x16];b.sampler.lod_bias=bef(r.data+0x18);b.map_id=b.tlut_id=map;
    if(!b.source.width||!b.source.height||!b.source.payload_address||ws>2||wt>2)return MH3_RES_INVALID;
    if(b.source.format>=8&&b.source.format<=10){b.source.indexed=1;p.resource_address=address;p.payload_address=be32(r.data+0x20);p.format=r.data[0xd];p.entries=be16(r.data+0xe);if(!p.payload_address||!p.entries)return MH3_RES_INVALID;}
    else if(b.source.format>6&&b.source.format!=14)return MH3_RES_UNSUPPORTED;
    *out=b;*palette=p;return MH3_RES_OK;
}
static void concat(const float a[12],const float b[12],float out[12]){
    float t[12];for(uint32_t r=0;r<3;r++)for(uint32_t c=0;c<4;c++)t[4*r+c]=a[4*r]*b[c]+a[4*r+1]*b[4+c]+a[4*r+2]*b[8+c]+(c==3?a[4*r+3]:0);
    memcpy(out,t,sizeof(t));
}
void mh3_effect_texture_matrix(const mh3_effect_texture_transform *p,float sine,float cosine,const float post[12],float out[12]){
    mh3_draw_identity_position(out);out[3]=p->offset[0];out[7]=p->offset[1];
    if(p->rotation!=0){float s=p->offset[0]-.5f,t=p->offset[1]-.5f;out[0]=out[5]=cosine;out[1]=-sine;out[4]=sine;out[3]=(.5f+cosine*s)-sine*t;out[7]=(.5f+sine*s)+cosine*t;}
    if(p->scale[0]!=1||p->scale[1]!=1||p->tile[0]!=1||p->tile[1]!=1||p->translation[0]!=0||p->translation[1]!=0){
        float s=p->scale[0]*p->tile[0],t=p->scale[1]*p->tile[1];out[0]*=s;out[1]*=s;out[3]=p->translation[0]+p->scale[0]*(.5f+(out[3]-.5f)*p->tile[0]);out[4]*=t;out[5]*=t;out[7]=p->translation[1]+p->scale[1]*(.5f+(out[7]-.5f)*p->tile[1]);
    }
    if(p->flip_s){out[0]*=-1;out[3]+=p->tile[0];}if(p->flip_t){out[5]*=-1;out[7]+=p->tile[1];}
    if(p->position_source)concat(post,out,out);
}
void mh3_effect_billboard_axes(mh3_effect_billboard *out,const float matrix[12],const float view[12],const float pivot[2],uint8_t kind){
    memcpy(out->matrix,matrix,48);memcpy(out->pivot,pivot,8);
    float a=sqrtf(matrix[0]*matrix[0]+matrix[4]*matrix[4]+matrix[8]*matrix[8]);
    float b=sqrtf(matrix[1]*matrix[1]+matrix[5]*matrix[5]+matrix[9]*matrix[9]);
    float length=sqrtf(view[1]*view[1]+view[5]*view[5]);float c=1,s=0;
    if(kind!=3&&length!=0){c=view[5]/length;s=-view[1]/length;}
    out->axis_x[0]=a*c;out->axis_x[1]=a*s;out->axis_y[0]=b*s;out->axis_y[1]=-b*c;
}
void mh3_effect_billboard_quad(const mh3_effect_billboard *d,const float position[3],float w,float h,float angle,float sine,float cosine,mh3_effect_quad *q){
    float center[3];for(uint32_t r=0;r<3;r++){const float *m=d->matrix+4*r;center[r]=angle==0?mh3_madd(m[2],position[2],m[0]*position[0])+mh3_madd(m[3],1,m[1]*position[1]):((m[0]*position[0]+m[1]*position[1])+m[2]*position[2])+m[3];}
    float ax=d->axis_x[0],ay=d->axis_x[1],bx=d->axis_y[0],by=d->axis_y[1],px=d->pivot[0],py=d->pivot[1];
    if(angle==0){
        float ox=-mh3_madd(w,px,-px),oy=-mh3_madd(h,py,-py),cx=mh3_madd(bx,oy,mh3_madd(ax,ox,center[0])),cy=mh3_madd(by,oy,mh3_madd(ay,ox,center[1]));
        float ux=ax*w,uy=ay*w,vx=bx*h,vy=by*h;
        q->position[0][0]=cx-(ux-vx);q->position[0][1]=cy-(uy-vy);q->position[1][0]=cx-(ux+vx);q->position[1][1]=cy-(uy+vy);
        q->position[2][0]=cx+(ux-vx);q->position[2][1]=cy+(uy-vy);q->position[3][0]=cx+(ux+vx);q->position[3][1]=cy+(uy+vy);
    }else{
        float cw=cosine*w,sw=sine*w,ch=cosine*h,sh=sine*h;
        float ox=(px-cw*px)-sh*py,oy=(py+sw*px)-ch*py,cx=center[0]+(ax*ox+bx*oy),cy=center[1]+(ay*ox+by*oy);
        float a_x=((ax*cw-ax*sh)-bx*sw)-bx*ch,a_y=((ay*cw-ay*sh)-by*sw)-by*ch;
        float b_x=((ax*cw+ax*sh)-bx*sw)+bx*ch,b_y=((ay*cw+ay*sh)-by*sw)+by*ch;
        q->position[0][0]=cx-a_x;q->position[0][1]=cy-a_y;q->position[1][0]=cx-b_x;q->position[1][1]=cy-b_y;
        q->position[2][0]=cx+a_x;q->position[2][1]=cy+a_y;q->position[3][0]=cx+b_x;q->position[3][1]=cy+b_y;
    }
    for(uint32_t i=0;i<4;i++)q->position[i][2]=i<2?center[2]:center[2]+0.0f;
}
void mh3_effect_quad_axes(const float c[3],const float a[3],const float b[3],mh3_effect_quad *q){for(uint32_t k=0;k<3;k++){q->position[0][k]=c[k]-a[k];q->position[1][k]=c[k]-b[k];q->position[2][k]=c[k]+a[k];q->position[3][k]=c[k]+b[k];}}
void mh3_effect_quad_transform(const float m[12],const mh3_effect_quad *in,mh3_effect_quad *out){mh3_effect_quad t;for(uint32_t i=0;i<4;i++)for(uint32_t r=0;r<3;r++)t.position[i][r]=((m[4*r]*in->position[i][0]+m[4*r+1]*in->position[i][1])+m[4*r+2]*in->position[i][2])+m[4*r+3];*out=t;}
void mh3_effect_movie_quad(int16_t x,int16_t y,int16_t w,int16_t h,int16_t inset,mh3_effect_quad *out){int16_t xs[4]={x,(int16_t)(x+w),(int16_t)(x+w),x},ys[4]={(int16_t)(y+inset),(int16_t)(y+inset),(int16_t)(y+h-inset),(int16_t)(y+h-inset)};for(uint32_t i=0;i<4;i++){out->position[i][0]=(float)xs[i];out->position[i][1]=(float)ys[i];out->position[i][2]=0;}}
void mh3_effect_quad_indices(uint16_t b,uint16_t out[6]){const uint16_t q[6]={0,1,2,0,2,3};for(uint32_t i=0;i<6;i++)out[i]=(uint16_t)(b+q[i]);}
void mh3_effect_strip_indices(uint16_t n,uint16_t *out){for(uint32_t i=2;i<n;i++){uint32_t j=3*(i-2);out[j]=(uint16_t)(i-2+(i&1));out[j+1]=(uint16_t)(i-1-(i&1));out[j+2]=(uint16_t)i;}}

void mh3_effect_draw_billboards(const mh3_effect_billboard *draw,float minimum,const mh3_effect_billboard_source *s){
    int first=1;for(uint32_t id=s->first(s->context);id;id=s->next(s->context,id)){
        mh3_effect_particle p;s->read(s->context,id,&p);
        float w=(p.size[0]*p.scale[0])*p.manager_scale[0],h;
        switch(p.emitter_flags&0x6000){case 0x4000:h=p.size[1]*p.scale[0];break;case 0x2000:h=p.size[0]*p.scale[1];break;case 0x6000:h=p.size[0]*p.scale[0];break;default:h=p.size[1]*p.scale[1];break;}
        h*=p.manager_scale[1];if(w<minimum||h<minimum)continue;
        s->prepare(s->context,id,first);first=0;if(s->orient)s->orient(s->context,id,&p);
        mh3_effect_quad quad;mh3_effect_billboard_quad(draw,p.position,w,h,p.rotation,p.sine,p.cosine,&quad);s->submit(s->context,&quad);
    }
}

static void movie_stage(mh3_pack_tev_stage *s,uint8_t coord,uint8_t map,mh3_pack_tev_pass color,mh3_pack_tev_pass alpha,uint8_t kc,uint8_t ka){
    memset(s,0,sizeof(*s));s->known=(UINT64_C(1)<<MH3_TEV_FIELD_COUNT)-1;s->texture_coordinate=coord;s->texture_map=map;s->raster_channel=255;s->color=color;s->alpha=alpha;s->konst_color=kc;s->konst_alpha=ka;
}
void mh3_effect_movie_material(const mh3_native_material_state *inherited,mh3_native_material_state *out){
    mh3_native_material_state m=*inherited;m.texgen_count=2;m.channel_count=0;m.tev_count=4;
    movie_stage(&m.tev[0],1,1,(mh3_pack_tev_pass){15,8,14,2,0,0,0,0,0},(mh3_pack_tev_pass){7,4,6,1,1,0,0,0,0},12,28);
    movie_stage(&m.tev[1],1,2,(mh3_pack_tev_pass){15,8,14,0,0,0,1,0,0},(mh3_pack_tev_pass){7,4,6,0,1,0,0,0,0},13,29);
    movie_stage(&m.tev[2],0,0,(mh3_pack_tev_pass){15,8,12,0,0,0,0,1,0},(mh3_pack_tev_pass){4,7,7,0,0,0,0,1,0},m.tev[2].konst_color,m.tev[2].konst_alpha);
    movie_stage(&m.tev[3],255,255,(mh3_pack_tev_pass){1,0,14,15,0,0,0,1,0},(mh3_pack_tev_pass){7,7,7,7,0,0,0,1,0},14,m.tev[3].konst_alpha);
    m.color[1]=(mh3_pack_tev_color){15,{-90,0,-114,135}};
    m.konst[0]=(mh3_pack_tev_color){15,{0,0,226,88}};m.konst[1]=(mh3_pack_tev_color){15,{179,0,0,182}};m.konst[2]=(mh3_pack_tev_color){15,{255,0,255,128}};
    m.swaps[0]=(mh3_pack_swap_table){15,{0,1,2,3}};
    pixel(&m.pixel,MH3_PIXEL_DEPTH_TEST,1);pixel(&m.pixel,MH3_PIXEL_DEPTH_COMPARE,7);pixel(&m.pixel,MH3_PIXEL_DEPTH_WRITE,0);
    pixel(&m.pixel,MH3_PIXEL_BLEND_ENABLE,0);pixel(&m.pixel,MH3_PIXEL_LOGIC_ENABLE,0);pixel(&m.pixel,MH3_PIXEL_BLEND_SUBTRACT,0);
    pixel(&m.pixel,MH3_PIXEL_BLEND_SOURCE,1);pixel(&m.pixel,MH3_PIXEL_BLEND_DESTINATION,0);pixel(&m.pixel,MH3_PIXEL_LOGIC_OPERATION,0);
    pixel(&m.pixel,MH3_PIXEL_COLOR_WRITE,1);pixel(&m.pixel,MH3_PIXEL_ALPHA_WRITE,0);
    for(uint32_t i=0;i<2;i++){m.texgen[i].known=(1u<<MH3_TEXGEN_FIELD_COUNT)-1;memset(m.texgen[i].values,0,sizeof(m.texgen[i].values));m.texgen[i].values[MH3_TEXGEN_PROJECTION]=0;m.texgen[i].values[MH3_TEXGEN_SOURCE_ROW]=5;m.texgen[i].values[MH3_TEXGEN_MATRIX_ROW]=60;m.texgen[i].values[MH3_TEXGEN_POST_MATRIX_ROW]=61;}
    *out=m;
}
void mh3_effect_movie_restore(const mh3_native_material_state *inherited,mh3_native_material_state *out){
    mh3_native_material_state m=*inherited;m.texgen_count=1;m.channel_count=0;m.tev_count=1;
    movie_stage(&m.tev[0],0,0,(mh3_pack_tev_pass){15,15,15,8,0,0,0,1,0},(mh3_pack_tev_pass){7,7,7,4,0,0,0,1,0},m.tev[0].konst_color,m.tev[0].konst_alpha);
    const uint8_t swaps[4][4]={{0,1,2,3},{0,0,0,3},{1,1,1,3},{2,2,2,3}};for(uint32_t i=0;i<4;i++){m.swaps[i].known=15;memcpy(m.swaps[i].rgba,swaps[i],4);m.tev[i].raster_swap=m.tev[i].texture_swap=0;}
    pixel(&m.pixel,MH3_PIXEL_DEPTH_TEST,1);pixel(&m.pixel,MH3_PIXEL_DEPTH_COMPARE,7);pixel(&m.pixel,MH3_PIXEL_DEPTH_WRITE,0);
    pixel(&m.pixel,MH3_PIXEL_BLEND_ENABLE,0);pixel(&m.pixel,MH3_PIXEL_LOGIC_ENABLE,0);pixel(&m.pixel,MH3_PIXEL_BLEND_SUBTRACT,0);
    pixel(&m.pixel,MH3_PIXEL_BLEND_SOURCE,1);pixel(&m.pixel,MH3_PIXEL_BLEND_DESTINATION,0);pixel(&m.pixel,MH3_PIXEL_LOGIC_OPERATION,15);*out=m;
}

void mh3_effect_draw_particles(float minimum,const mh3_effect_billboard_source *s,mh3_effect_particle_draw draw){
    int first=1;for(uint32_t id=s->first(s->context);id;id=s->next(s->context,id)){
        mh3_effect_particle p;s->read(s->context,id,&p);float w=(p.size[0]*p.scale[0])*p.manager_scale[0],h;
        switch(p.emitter_flags&0x6000){case 0x4000:h=p.size[1]*p.scale[0];break;case 0x2000:h=p.size[0]*p.scale[1];break;case 0x6000:h=p.size[0]*p.scale[0];break;default:h=p.size[1]*p.scale[1];break;}
        h*=p.manager_scale[1];if(w<minimum||(!s->width_only&&h<minimum))continue;s->prepare(s->context,id,first);first=0;draw(s->context,id,&p,w,h);
    }
}

void mh3_effect_material_vertices(const mh3_draw_state *d,const mh3_effect_quad *q,const float uv[4][2],const mh3_effect_vertex_inputs *in,AuroraNativeMaterialVertex out[4]){
    static const uint8_t white[4]={255,255,255,255};
    for(uint32_t i=0;i<4;i++){
        AuroraNativeMaterialVertex *v=out+i;memset(v,0,sizeof(*v));AuroraNativeVertex clip;mh3_draw_vertex(d,q->position[i],uv[i],white,&clip);memcpy(v->position,clip.position,16);
        for(uint32_t r=0;r<3;r++)v->view_position[r]=((d->position[4*r]*q->position[i][0]+d->position[4*r+1]*q->position[i][1])+d->position[4*r+2]*q->position[i][2])+d->position[4*r+3];
        memcpy(v->normal,in->normal,12);memcpy(v->color,in->color,8);
        for(uint32_t slot=0;slot<8;slot++){
            const float *m=in->texture_matrix[slot];float source[3]={uv[i][0],uv[i][1],0};if(in->position_source[slot])memcpy(source,q->position[i],12);
            for(uint32_t r=0;r<3;r++)v->texcoord[slot][r]=((m[4*r]*source[0]+m[4*r+1]*source[1])+m[4*r+2]*source[2])+m[4*r+3];
            if(!in->position_source[slot])v->texcoord[slot][2]=1;
        }
    }
}
void mh3_effect_expand_point(const mh3_draw_state *d,float width,AuroraNativeMaterialVertex v[4]){
    float vw=d->viewport[2],vh=d->viewport[3];if(vw<=0||vh<=0)return;
    for(uint32_t i=0;i<4;i++){v[i].position[0]+=((i&1)?width:-width)/vw*v[i].position[3];v[i].position[1]+=(i>=2?width:-width)/vh*v[i].position[3];}
}
void mh3_effect_expand_line(const mh3_draw_state *d,float width,AuroraNativeMaterialVertex v[4]){
    float vw=d->viewport[2],vh=d->viewport[3],wa=v[0].position[3],wb=v[2].position[3];if(vw<=0||vh<=0||wa==0||wb==0)return;
    float x=(v[2].position[0]/wb-v[0].position[0]/wa)*vw,y=(v[2].position[1]/wb-v[0].position[1]/wa)*vh,n=sqrtf(x*x+y*y);if(n>1e-5f){x/=n;y/=n;}else{x=1;y=0;}
    for(uint32_t i=0;i<4;i++){float sign=(i&1)?1:-1;v[i].position[0]+=-y*width/vw*sign*v[i].position[3];v[i].position[1]+=x*width/vh*sign*v[i].position[3];}
}
static void cross(const float a[3],const float b[3],float out[3]){float v[3]={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};memcpy(out,v,12);}
static int normalize(float v[3]){float n=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(n==0)return 0;for(uint32_t i=0;i<3;i++)v[i]/=n;return 1;}
void mh3_effect_directional_matrix(const mh3_effect_directional_inputs *in,float history[3],float out[12]){
    float a[3],d[3],z[3],basis[12];memcpy(d,in->direction,12);
    if(in->screen_facing){
        float raw[3];for(uint32_t r=0;r<3;r++)raw[r]=(in->view_local[4*r]*d[0]+in->view_local[4*r+1]*d[1])+in->view_local[4*r+2]*d[2];memcpy(d,raw,12);normalize(d);
        a[0]=raw[1];a[1]=-raw[0];a[2]=0;
        if(!normalize(a)){cross(raw,in->history,a);if(!normalize(a))memcpy(a,in->fallback,12);cross(a,raw,z);}else{z[0]=-raw[0]*raw[2];z[1]=-raw[1]*raw[2];z[2]=raw[0]*raw[0]+raw[1]*raw[1];}normalize(z);memcpy(history,z,12);
        for(uint32_t r=0;r<3;r++){float scale=sqrtf(in->view_local[r]*in->view_local[r]+in->view_local[4+r]*in->view_local[4+r]+in->view_local[8+r]*in->view_local[8+r]);for(uint32_t row=0;row<3;row++)basis[4*row+r]=(r==0?a[row]:r==1?d[row]:z[row])*scale;}
        for(uint32_t r=0;r<3;r++)basis[4*r+3]=((in->view_local[4*r]*in->position[0]+in->view_local[4*r+1]*in->position[1])+in->view_local[4*r+2]*in->position[2])+in->view_local[4*r+3];
    }else{
        cross(in->history,d,z);if(!normalize(z))memcpy(z,in->fallback,12);cross(d,z,a);memcpy(history,a,12);
        for(uint32_t r=0;r<3;r++){basis[4*r]=a[r];basis[4*r+1]=d[r];basis[4*r+2]=z[r];basis[4*r+3]=in->position[r];}
    }
    concat(basis,in->size_matrix,out);
}
static void ribbon_append(const mh3_effect_ribbon_source *s,uint32_t particle,int origin,float fraction,float previous[2][3],float *previous_uv,int *have_previous){
    float pair[2][3];s->pair(s->context,particle,origin,pair);
    if(*have_previous){mh3_effect_quad q;float uv[4][2]={{1,*previous_uv},{0,*previous_uv},{1,fraction},{0,fraction}};memcpy(q.position[0],previous[0],12);memcpy(q.position[1],previous[1],12);memcpy(q.position[2],pair[0],12);memcpy(q.position[3],pair[1],12);s->submit(s->context,&q,uv);}
    memcpy(previous,pair,24);*previous_uv=fraction;*have_previous=1;
}
void mh3_effect_draw_ribbon(const mh3_effect_ribbon_source *s){
    uint32_t count=s->count+(s->closure==1||s->closure==2),first=s->first(s->context);if(!first||count<2)return;
    float previous[2][3],previous_uv=0;int have=0,index=s->reverse?(int)count-1:0,step=s->reverse?-1:1;float scale=s->individual_uv?1:1.0f/(float)(count-1);
    if(s->closure==2&&!s->reverse){ribbon_append(s,0,1,scale*(float)index,previous,&previous_uv,&have);index+=step;}
    for(uint32_t p=first;p;p=s->next(s->context,p)){ribbon_append(s,p,0,scale*(float)index,previous,&previous_uv,&have);index+=step;}
    if(s->closure==1)ribbon_append(s,first,0,scale*(float)index,previous,&previous_uv,&have);
    if(s->closure==2&&s->reverse)ribbon_append(s,0,1,scale*(float)index,previous,&previous_uv,&have);
}
void mh3_effect_tube_quad(const mh3_effect_tube_frame f[2],const float angles[2][2],uint32_t sample,uint32_t segments,int reverse,mh3_effect_quad *out,float uv[4][2]){
    for(uint32_t i=0;i<4;i++){uint32_t end=(i&1)^(reverse!=0),angle=i/2;for(uint32_t k=0;k<3;k++)out->position[i][k]=(f[end].axis_y[k]*angles[angle][1]+f[end].axis_z[k]*angles[angle][0])+f[end].center[k];uv[i][0]=(float)(sample+angle)/(float)segments;uv[i][1]=f[end].texture_t;}
}

/* Quadratic B-spline used by the NW4R smooth stripe strategy. Each operation
 * retains the single-precision boundary present in the original PPC helper. */
void mh3_effect_spline_weights(float t,float out[3]) {
    float square=t*t,half=0.5f*square;
    out[0]=0.5f+(half-t);out[1]=0.5f+(-square+t);out[2]=half;
}
void mh3_effect_spline_pair(const mh3_effect_ribbon_pair frames[3],const float w[3],
                            mh3_effect_ribbon_pair *out) {
    for(unsigned side=0;side<2;side++)for(unsigned axis=0;axis<3;axis++)
        out->position[side][axis]=(frames[0].position[side][axis]*w[0]+frames[1].position[side][axis]*w[1])+frames[2].position[side][axis]*w[2];
    out->texture_t=(frames[0].texture_t*w[0]+frames[1].texture_t*w[1])+frames[2].texture_t*w[2];
}
void mh3_effect_spline_tube(const mh3_effect_tube_frame frames[3],const float w[3],
                            mh3_effect_tube_frame *out) {
    for(unsigned axis=0;axis<3;axis++) {
        out->center[axis]=(frames[0].center[axis]*w[0]+frames[1].center[axis]*w[1])+frames[2].center[axis]*w[2];
        out->axis_y[axis]=(frames[0].axis_y[axis]*w[0]+frames[1].axis_y[axis]*w[1])+frames[2].axis_y[axis]*w[2];
        out->axis_z[axis]=(frames[0].axis_z[axis]*w[0]+frames[1].axis_z[axis]*w[1])+frames[2].axis_z[axis]*w[2];
    }
    out->texture_t=(frames[0].texture_t*w[0]+frames[1].texture_t*w[1])+frames[2].texture_t*w[2];
}
typedef struct smooth_output {
    const mh3_effect_ribbon_source *source;
    mh3_effect_ribbon_pair previous;
    int have_previous;
} smooth_output;
static void smooth_emit(smooth_output *stream,const mh3_effect_ribbon_pair *pair) {
    if(stream->have_previous) {
        mh3_effect_quad quad;float uv[4][2];
        for(unsigned side=0;side<2;side++) {
            memcpy(quad.position[side],stream->previous.position[side],sizeof(quad.position[side]));
            memcpy(quad.position[side+2],pair->position[side],sizeof(quad.position[side]));
            uv[side][0]=uv[side+2][0]=side?0:1;
            uv[side][1]=stream->previous.texture_t;uv[side+2][1]=pair->texture_t;
        }
        stream->source->submit(stream->source->context,&quad,uv);
    }
    stream->previous=*pair;stream->have_previous=1;
}
static void smooth_segment(smooth_output *stream,uint32_t count,const mh3_effect_ribbon_pair *a,
                           const mh3_effect_ribbon_pair *b,const mh3_effect_ribbon_pair *c,int endpoint) {
    mh3_effect_ribbon_pair frames[3]={*a,*b,*c},out;float weights[3];
    if(endpoint){mh3_effect_spline_weights(1,weights);mh3_effect_spline_pair(frames,weights,&out);smooth_emit(stream,&out);return;}
    float step=1.0f/(float)count;
    for(uint32_t i=0;i<count;i++){mh3_effect_spline_weights((float)i*step,weights);mh3_effect_spline_pair(frames,weights,&out);smooth_emit(stream,&out);}
}
void mh3_effect_draw_smooth_ribbon(const mh3_effect_smooth_ribbon_source *input) {
    const mh3_effect_ribbon_source *s=&input->ribbon;
    if(!s->count||!input->subdivisions)return;
    uint32_t particle=s->first(s->context);if(!particle)return;
    int reverse=s->reverse!=0,step=reverse?-1:1;
    float scale=s->individual_uv?1.0f:1.0f/(float)(s->count-(s->closure==0));
    int32_t index=reverse?(int32_t)s->count-(s->closure==0):0;
    if(s->closure==1){scale=s->individual_uv?0.5f:0.5f/(float)s->count;index=reverse?(int32_t)s->count*2+1:-1;step=reverse?-2:2;}
    mh3_effect_ribbon_pair a,b,c,first,second;
    smooth_output stream={s,{{{0}},0},0};
    s->pair(s->context,particle,0,b.position);b.texture_t=(float)index*scale;first=b;a=b;
    index+=step;
    if(s->closure==2&&!reverse) {
        c=b;c.texture_t=(float)index*scale;
        for(unsigned side=0;side<2;side++)for(unsigned axis=0;axis<3;axis++)b.position[side][axis]+=input->origin_delta[axis];
        a=b;smooth_segment(&stream,input->subdivisions,&a,&b,&c,0);a=b;b=c;index+=step;
    }
    particle=s->next(s->context,particle);
    if(s->closure==1) {
        if(!particle)return;
        s->pair(s->context,particle,0,c.position);c.texture_t=(float)index*scale;second=c;
        a=b;b=c;index+=step;particle=s->next(s->context,particle);
    }
    for(;particle;particle=s->next(s->context,particle)) {
        s->pair(s->context,particle,0,c.position);c.texture_t=(float)index*scale;
        smooth_segment(&stream,input->subdivisions,&a,&b,&c,0);a=b;b=c;index+=step;
    }
    if(s->closure==1) {
        first.texture_t=(float)index*scale;
        smooth_segment(&stream,input->subdivisions,&a,&b,&first,0);index+=step;
        second.texture_t=(float)index*scale;
        smooth_segment(&stream,input->subdivisions,&b,&first,&second,0);
        smooth_segment(&stream,input->subdivisions,&b,&first,&second,1);
    } else {
        if(s->closure==2&&reverse) {
            c=b;c.texture_t=(float)index*scale;
            for(unsigned side=0;side<2;side++)for(unsigned axis=0;axis<3;axis++)c.position[side][axis]+=input->origin_delta[axis];
            smooth_segment(&stream,input->subdivisions,&a,&b,&c,0);a=b;b=c;
        }
        smooth_segment(&stream,input->subdivisions,&a,&b,&b,0);
        smooth_segment(&stream,input->subdivisions,&a,&b,&b,1);
    }
}
