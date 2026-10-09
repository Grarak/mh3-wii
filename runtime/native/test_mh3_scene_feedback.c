#include "mh3_scene_feedback.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static void near(float a,float b){assert(fabsf(a-b)<.000001f);}
int main(void){
    mh3_draw_state draw={0};mh3_native_material_state material={0};draw.raster.alpha_write=1;material.indirect_count=2;
    mh3_temporal_feedback_input input={0};input.identity=0x81000000;input.screen_width=640;input.screen_height=448;input.projection_height=448;input.enabled=1;input.alpha=128;input.mode=3;input.amount=128;input.offset[0]=.5f;input.threshold_mode=2;input.threshold_alpha=100;input.threshold_gray=80;
    mh3_temporal_feedback_plan plan;assert(mh3_scene_temporal_prepare(&input,&draw,&material,&plan));assert(plan.frames==1&&!plan.draw_previous&&plan.draw_threshold);assert(plan.capture.width==320&&plan.capture.height==224&&plan.capture.box_filter);assert(plan.previous.material.indirect_count==2);near(plan.previous.uv[0][0],.1875f);near(plan.previous.uv[1][0],.9375f);assert(plan.previous.raster.depth_test==0&&plan.previous.raster.alpha_write==1&&plan.threshold.raster.alpha_write==0);assert(plan.threshold.material.tev[0].color.scale==1);
    input.frames=1;assert(mh3_scene_temporal_prepare(&input,&draw,&material,&plan)&&plan.draw_previous);input.mode=1;input.subtract_scale=2;assert(mh3_scene_temporal_prepare(&input,&draw,&material,&plan));assert(plan.previous.material.tev[0].color.operation==1&&plan.previous.material.tev[0].color.scale==2&&plan.previous.raster.dst_factor==1);
    input.mode=2;assert(mh3_scene_temporal_prepare(&input,&draw,&material,&plan));assert(plan.previous.raster.blend_mode==3&&plan.previous.raster.dst_factor==5);
    input.enabled=0;input.frames=123;assert(mh3_scene_temporal_prepare(&input,&draw,&material,&plan)&&!plan.enabled&&plan.frames==123);
    mh3_scene_quad_plan q;assert(mh3_scene_capture_quad(1,640,448,640,448,120,1,&draw,&material,&q));assert(q.color[0]==120&&q.color[3]==255&&q.raster.blend_mode==3&&q.raster.src_factor==2&&q.material.channel[0].color.material_source==1);
    mh3_scene_glare_input glare={.identity=1,.shift=1,.low_threshold=64,.high_threshold=192,.levels=3,.start_width=320,.start_height=224,.darken=.5f,.brighten=.5f,.projection_width=640,.projection_height=448};mh3_scene_glare_plan bloom;assert(mh3_scene_glare_prepare(&glare,&bloom));assert(bloom.level_count==3&&bloom.width==80&&bloom.height==56);assert(bloom.levels[0].source_width==640&&bloom.levels[0].width==320&&bloom.levels[2].width==80);near(bloom.shift_scale,.5f);
    const float dark[3]={.1f,.1f,.1f},bright[3]={.9f,.9f,.9f},red[3]={1,0,0};near(mh3_scene_glare_brightness(&bloom,dark),.025f);near(mh3_scene_glare_brightness(&bloom,bright),.5f);near(mh3_scene_glare_brightness(&bloom,red),77.0f/512);
    memset(&bloom,0x5a,sizeof(bloom));mh3_scene_glare_plan unchanged=bloom;glare.levels=16;assert(!mh3_scene_glare_prepare(&glare,&bloom)&&!memcmp(&bloom,&unchanged,sizeof(bloom)));glare.levels=-1;assert(!mh3_scene_glare_prepare(&glare,&bloom));
    /* Source samples operate on authored tiled bytes, including transparent
     * vertex modulation and indexed palettes, independently of framebuffer. */
    AuroraNativeVertex vertices[4]={0};const float clip[4][2]={{-1,1},{1,1},{1,-1},{-1,-1}},uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
    for(unsigned i=0;i<4;i++){vertices[i].position[0]=clip[i][0];vertices[i].position[1]=clip[i][1];vertices[i].position[3]=1;memcpy(vertices[i].uv,uv[i],8);memset(vertices[i].color,255,4);}draw.viewport[2]=draw.viewport[3]=4;
    mh3_native_texture_binding texture={0};texture.source.width=texture.source.height=4;texture.source.format=6;uint8_t pixels[64]={0};
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){const unsigned i=y*4+x;pixels[i*2]=255;pixels[i*2+1]=x==0?255:0;pixels[32+i*2]=x==1?255:0;pixels[33+i*2]=x==2?255:0;}
    float sampled[4],point[2]={.5f,.5f};assert(mh3_scene_source_quad((mh3_res_span){pixels,64},(mh3_res_span){0},0,&texture,vertices,&draw,point,sampled)==1);near(sampled[0],1);near(sampled[1],0);near(sampled[3],1);
    texture.sampler.mag_filter=1;point[0]=1;assert(mh3_scene_source_quad((mh3_res_span){pixels,64},(mh3_res_span){0},0,&texture,vertices,&draw,point,sampled)==1);near(sampled[0],.5f);near(sampled[1],.5f);
    point[0]=5;assert(mh3_scene_source_quad((mh3_res_span){pixels,64},(mh3_res_span){0},0,&texture,vertices,&draw,point,sampled)==0);
    uint8_t indices[32],palette[4]={0,0,0,31};memset(indices,0x11,32);texture.source.format=8;point[0]=.5f;assert(mh3_scene_source_quad((mh3_res_span){indices,32},(mh3_res_span){palette,4},1,&texture,vertices,&draw,point,sampled)==1);near(sampled[2],1);near(sampled[0],0);
    assert(mh3_scene_source_quad((mh3_res_span){indices,16},(mh3_res_span){palette,4},1,&texture,vertices,&draw,point,sampled)==-1);
    puts("native scene feedback and authored source sampling invariants pass");return 0;
}
