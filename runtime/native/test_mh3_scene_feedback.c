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
    puts("native scene feedback invariants pass");return 0;
}
