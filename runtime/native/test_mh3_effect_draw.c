/* Focused invariants for the native particle producer, independent of GX/GPU. */
#include "mh3_effect_draw.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void near(float a,float b){assert(fabsf(a-b)<.00002f);}
static void put16(uint8_t *p,uint16_t x){p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static void put32(uint8_t *p,uint32_t x){p[0]=(uint8_t)(x>>24);p[1]=(uint8_t)(x>>16);p[2]=(uint8_t)(x>>8);p[3]=(uint8_t)x;}
static void material(void){
    uint8_t r[0xac]={};put16(r,0x13);r[5]=1;r[8]=0;r[0xc]=15;r[0xd]=8;r[0xe]=14;r[0xf]=15;r[0x1f]=1;r[0x30]=7;r[0x31]=4;r[0x32]=6;r[0x33]=7;r[0x43]=1;r[0x5c]=1;r[0x5d]=4;r[0x5e]=5;r[0x70]=3;
    mh3_effect_material m;assert(mh3_effect_material_read((mh3_res_span){r,sizeof(r)},&m)==MH3_RES_OK);assert(m.texture_count==1&&m.texture_maps[0]==0&&m.texture_maps[1]==-1);assert(m.stages[0].color.b==8&&m.stages[0].texture_map==0);
    mh3_native_material_state inherited={};inherited.pixel.known=UINT64_C(1)<<MH3_PIXEL_ALPHA_WRITE;inherited.pixel.values[MH3_PIXEL_ALPHA_WRITE]=1;inherited.konst[3].known=15;inherited.konst[3].rgba[0]=123;mh3_native_material_state out;
    mh3_effect_material_apply(&m,&inherited,&out);assert(out.pixel.values[MH3_PIXEL_ALPHA_WRITE]==1&&out.konst[3].rgba[0]==123);assert(out.pixel.values[MH3_PIXEL_BLEND_SOURCE]==4&&out.tev_count==1);
    memset(&m,0x5a,sizeof(m));mh3_effect_material sentinel=m;r[5]=5;assert(mh3_effect_material_read((mh3_res_span){r,sizeof(r)},&m)==MH3_RES_INVALID);assert(!memcmp(&m,&sentinel,sizeof(m)));assert(mh3_effect_material_read((mh3_res_span){r,sizeof(r)-1},&m)==MH3_RES_RANGE);
    uint8_t colors[4][4]={{255,128,0,255},{255,128,200,128},{20,40,60,80},{100,150,200,250}},rgb[3];mh3_effect_color(5,colors,rgb);assert(rgb[0]==254&&rgb[1]==64&&rgb[2]==0);assert(mh3_effect_alpha(5,colors)==128);mh3_effect_color(6,colors,rgb);assert(rgb[0]==8&&rgb[1]==23&&rgb[2]==47);
}
static void texture(void){
    uint8_t bytes[0x24]={};put16(bytes+4,16);put16(bytes+6,8);bytes[0xc]=9;bytes[0xd]=2;put16(bytes+0xe,256);bytes[0x14]=3;bytes[0x15]=5;bytes[0x16]=1;put32(bytes+0x1c,0x81000000);put32(bytes+0x20,0x81001000);
    mh3_native_texture_binding binding;mh3_native_palette_source palette;assert(mh3_effect_texture_read((mh3_res_span){bytes,sizeof(bytes)},0x81002000,2,2,1,&binding,&palette)==MH3_RES_OK);assert(binding.source.indexed&&binding.source.max_lod==2&&binding.map_id==2&&binding.sampler.wrap_s==2);assert(palette.entries==256&&palette.format==2&&palette.payload_address==0x81001000);
    bytes[0xc]=7;assert(mh3_effect_texture_read((mh3_res_span){bytes,sizeof(bytes)},0,0,0,0,&binding,&palette)==MH3_RES_UNSUPPORTED);
    mh3_effect_texture_transform t={{1,1},{0,0},{1,1},{0,0},0,0,0,0};float post[12],m[12];mh3_draw_identity_position(post);mh3_effect_texture_matrix(&t,0,1,post,m);near(m[0],1);near(m[5],1);near(m[3],0);
    t.rotation=1;mh3_effect_texture_matrix(&t,1,0,post,m);near(m[0],0);near(m[1],-1);near(m[3],1);near(m[4],1);near(m[7],0);
    t.rotation=0;t.scale[0]=-2;t.translation[0]=2;t.tile[0]=.25f;t.offset[0]=.25f;mh3_effect_texture_matrix(&t,0,1,post,m);near(m[0],-.5f);near(m[3],1.125f);
    t.flip_s=1;mh3_effect_texture_matrix(&t,0,1,post,m);near(m[0],.5f);near(m[3],1.375f);
}
static void geometry(void){
    float id[12];mh3_draw_identity_position(id);float pivot[2]={0,0},position[3]={10,20,-30};mh3_effect_billboard b;mh3_effect_billboard_axes(&b,id,id,pivot,3);mh3_effect_quad q;
    mh3_effect_billboard_quad(&b,position,2,3,0,0,1,&q);near(q.position[0][0],8);near(q.position[0][1],17);near(q.position[1][1],23);near(q.position[2][0],12);near(q.position[3][2],-30);
    mh3_effect_billboard_quad(&b,position,2,3,1,1,0,&q);near(q.position[0][0],13);near(q.position[0][1],18);near(q.position[1][0],7);near(q.position[2][0],7);near(q.position[3][0],13);
    uint16_t indices[6];mh3_effect_quad_indices(4,indices);const uint16_t expected[6]={4,5,6,4,6,7};assert(!memcmp(indices,expected,12));mh3_effect_strip_indices(4,indices);const uint16_t strip[6]={0,1,2,2,1,3};assert(!memcmp(indices,strip,12));
    mh3_effect_movie_quad(-30000,-100,32000,200,4,&q);near(q.position[0][1],-96);near(q.position[2][1],96);near(q.position[1][0],2000);
    mh3_draw_state draw={};mh3_draw_identity_position(draw.position);assert(mh3_draw_ortho(draw.projection,0,240,0,320,0,-1));draw.viewport[2]=320;draw.viewport[3]=240;
    mh3_effect_vertex_inputs in={};in.normal[0]=1;for(unsigned i=0;i<8;i++)mh3_draw_identity_position(in.texture_matrix[i]);in.texture_matrix[1][0]=2;in.texture_matrix[1][3]=.5f;in.position_source[2]=1;in.texture_matrix[2][8]=.25f;in.texture_matrix[2][11]=1;
    const float uv[4][2]={{0,0},{1,0},{1,1},{0,1}};mh3_effect_movie_quad(0,0,320,240,0,&q);AuroraNativeMaterialVertex vertices[4];mh3_effect_material_vertices(&draw,&q,uv,&in,vertices);near(vertices[0].position[0],-1);near(vertices[0].position[1],1);near(vertices[1].texcoord[1][0],2.5f);near(vertices[2].texcoord[2][2],81);near(vertices[0].normal[0],1);
    for(unsigned i=0;i<4;i++)vertices[i]=vertices[0];mh3_effect_expand_point(&draw,6,vertices);near(vertices[1].position[0]-vertices[0].position[0],12.0f/320);near(vertices[2].position[1]-vertices[0].position[1],12.0f/240);
}
typedef struct {unsigned prepared,oriented,submitted,order[3];} walk;
static uint32_t first(void *p){(void)p;return 1;}static uint32_t next(void *p,uint32_t n){(void)p;return n==3?0:n+1;}
static void read_particle(void *p,uint32_t n,mh3_effect_particle *out){(void)p;memset(out,0,sizeof(*out));out->size[0]=n==1?0:2;out->size[1]=3;out->scale[0]=out->scale[1]=out->manager_scale[0]=out->manager_scale[1]=1;out->cosine=1;}
static void prepare(void *p,uint32_t n,int first_draw){walk *w=p;assert(first_draw==(w->prepared==0));w->order[w->prepared++]=n;}
static void orient(void *p,uint32_t n,mh3_effect_particle *out){(void)n;(void)out;((walk*)p)->oriented++;}static void submit(void *p,const mh3_effect_quad *q){near(q->position[0][0],-2);((walk*)p)->submitted++;}
static void traversal(void){walk w={};mh3_effect_billboard_source source={&w,first,next,read_particle,prepare,orient,submit,0};float id[12],pivot[2]={0,0};mh3_draw_identity_position(id);mh3_effect_billboard b;mh3_effect_billboard_axes(&b,id,id,pivot,3);mh3_effect_draw_billboards(&b,.1f,&source);assert(w.prepared==2&&w.oriented==2&&w.submitted==2&&w.order[0]==2&&w.order[1]==3);}
static void movie(void){mh3_native_material_state inherited={},m;inherited.cull_mode=2;inherited.indirect_count=1;mh3_effect_movie_material(&inherited,&m);assert(m.cull_mode==2&&m.indirect_count==1&&m.tev_count==4);assert(m.texgen[0].values[MH3_TEXGEN_PROJECTION]==0&&m.texgen[1].values[MH3_TEXGEN_PROJECTION]==0);assert(m.color[1].rgba[0]==-90&&m.color[1].rgba[2]==-114&&m.konst[1].rgba[3]==182);assert(m.tev[0].texture_map==1&&m.tev[1].texture_map==2&&m.tev[2].texture_map==0&&m.tev[3].color.a==1);mh3_effect_movie_restore(&m,&m);assert(m.tev_count==1&&m.texgen_count==1&&m.pixel.values[MH3_PIXEL_ALPHA_WRITE]==0&&m.pixel.values[MH3_PIXEL_LOGIC_OPERATION]==15);}
typedef struct stripe_walk {unsigned reverse,pairs,submitted;float first_x,last_x,first_t,last_t;} stripe_walk;
static uint32_t stripe_first(void *opaque){stripe_walk *w=opaque;return w->reverse?3:1;}
static uint32_t stripe_next(void *opaque,uint32_t p){stripe_walk *w=opaque;return w->reverse?(p==1?0:p-1):(p==3?0:p+1);}
static void stripe_pair(void *opaque,uint32_t p,int origin,float out[2][3]){
    stripe_walk *w=opaque;w->pairs++;
    for(unsigned side=0;side<2;side++){out[side][0]=origin?0:(float)p*10;out[side][1]=side?-1:1;out[side][2]=0;}
}
static void stripe_submit(void *opaque,const mh3_effect_quad *q,const float uv[4][2]){
    stripe_walk *w=opaque;if(!w->submitted){w->first_x=q->position[0][0];w->first_t=uv[0][1];}
    w->last_x=q->position[2][0];w->last_t=uv[2][1];w->submitted++;
    near(uv[0][0],1);near(uv[1][0],0);near(q->position[0][1],1);near(q->position[1][1],-1);
}
static void stripes(void){
    float weights[3];mh3_effect_spline_weights(.5f,weights);near(weights[0],.125f);near(weights[1],.75f);near(weights[2],.125f);
    for(unsigned reverse=0;reverse<2;reverse++)for(unsigned closure=0;closure<3;closure++){
        stripe_walk w={reverse,0,0,0,0,0,0};mh3_effect_ribbon_source ribbon={&w,stripe_first,stripe_next,stripe_pair,stripe_submit,3,(uint8_t)closure,(uint8_t)reverse,0};
        mh3_effect_draw_ribbon(&ribbon);assert(w.submitted==(closure?3:2));near(w.first_t,reverse?1:0);near(w.last_t,reverse?0:1);assert(w.pairs==(closure?4:3));
        w=(stripe_walk){reverse,0,0,0,0,0,0};mh3_effect_smooth_ribbon_source smooth={ribbon,2,{100,0,0}};
        mh3_effect_draw_smooth_ribbon(&smooth);assert(w.submitted==(closure==2?8:6));assert(w.pairs==3);near(w.first_t,reverse?1:0);near(w.last_t,reverse?0:1);
        if(closure==1){near(w.first_x,reverse?25:15);near(w.last_x,w.first_x);}
        else if(closure==0){near(w.first_x,reverse?30:10);near(w.last_x,reverse?10:30);}
        else {near(w.first_x,reverse?30:110);near(w.last_x,reverse?110:30);}
    }
    mh3_effect_tube_frame control[3]={0},interpolated;
    for(unsigned i=0;i<3;i++){control[i].center[0]=(float)i*10;control[i].axis_y[1]=2;control[i].axis_z[2]=3;control[i].texture_t=(float)i*.5f;}
    mh3_effect_spline_weights(.5f,weights);mh3_effect_spline_tube(control,weights,&interpolated);near(interpolated.center[0],10);near(interpolated.texture_t,.5f);
    mh3_effect_tube_frame ends[2]={control[0],control[2]};const float angles[2][2]={{1,0},{0,1}};mh3_effect_quad q;float uv[4][2];
    mh3_effect_tube_quad(ends,angles,1,4,0,&q,uv);near(q.position[0][2],3);near(q.position[1][0],20);near(q.position[2][1],2);near(uv[0][0],.25f);near(uv[2][0],.5f);near(uv[1][1],1);
    mh3_effect_tube_quad(ends,angles,1,4,1,&q,uv);near(q.position[0][0],20);near(uv[0][1],1);
    mh3_effect_directional_inputs d={0};mh3_draw_identity_position(d.view_local);mh3_draw_identity_position(d.size_matrix);d.direction[1]=1;d.history[0]=1;d.fallback[2]=1;d.position[0]=7;
    float history[3],matrix[12];mh3_effect_directional_matrix(&d,history,matrix);near(matrix[0],1);near(matrix[5],1);near(matrix[10],1);near(matrix[3],7);near(history[0],1);
    d.screen_facing=1;mh3_effect_directional_matrix(&d,history,matrix);near(matrix[0],1);near(matrix[5],1);near(history[2],1);near(matrix[3],7);
    d.direction[1]=0;d.direction[2]=1;mh3_effect_directional_matrix(&d,history,matrix);assert(isfinite(matrix[0]));near(matrix[9],1);
}
int main(void){material();texture();geometry();traversal();movie();stripes();puts("native effect invariants pass");}
