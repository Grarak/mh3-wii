#ifndef MH3_SCENE_FEEDBACK_H
#define MH3_SCENE_FEEDBACK_H
#include "mh3_draw_state.h"
#include "mh3_model_state.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mh3_scene_capture_plan {
    uint32_t identity,width,height,format;
    float source[4]; /* logical top-left x,y,width,height */
    uint8_t box_filter;
} mh3_scene_capture_plan;
typedef struct mh3_scene_quad_plan {
    float projection[16],position[12],vertices[4][3],uv[4][2];
    uint8_t color[4];
    mh3_raster_state raster;
    mh3_native_material_state material;
    uint32_t identity,width,height,format;
} mh3_scene_quad_plan;
typedef struct mh3_temporal_feedback_input {
    uint32_t identity,frames;
    uint16_t screen_width,screen_height;
    float projection_height,offset[2];
    uint8_t enabled,mode,alpha,amount,threshold_mode,threshold_alpha,threshold_gray,subtract_scale;
    uint8_t tint[3];
} mh3_temporal_feedback_input;
typedef struct mh3_temporal_feedback_plan {
    uint32_t frames;
    uint8_t enabled,draw_previous,draw_threshold;
    mh3_scene_capture_plan capture;
    mh3_scene_quad_plan initial,previous,threshold;
} mh3_temporal_feedback_plan;
int mh3_scene_temporal_prepare(const mh3_temporal_feedback_input *input,
                               const mh3_draw_state *draw,const mh3_native_material_state *material,
                               mh3_temporal_feedback_plan *out);
int mh3_scene_capture_quad(uint32_t identity,float texture_width,float texture_height,
                           float screen_width,float screen_height,uint8_t tint,int subtract,
                           const mh3_draw_state *draw,const mh3_native_material_state *material,
                           mh3_scene_quad_plan *out);
typedef struct mh3_scene_glare_input {
    uint32_t identity,shift,low_threshold,high_threshold;
    int32_t levels;
    int16_t start_width,start_height;
    float darken,brighten,projection_width,projection_height;
} mh3_scene_glare_input;
enum { MH3_SCENE_GLARE_LEVELS=16 };
typedef struct mh3_scene_glare_level {
    uint16_t source_width,source_height,width,height;
    int16_t draw_width,draw_height;
} mh3_scene_glare_level;
typedef struct mh3_scene_glare_plan {
    uint32_t identity,level_count;
    uint16_t width,height;
    float projection_width,projection_height;
    float low_threshold,high_threshold,darken,brighten,shift_scale;
    mh3_scene_glare_level levels[MH3_SCENE_GLARE_LEVELS];
} mh3_scene_glare_plan;
int mh3_scene_glare_prepare(const mh3_scene_glare_input *input,mh3_scene_glare_plan *out);
/* Float material policy: authored threshold/boost equations without TEV byte
 * quantization. This reference never reads any GPU pixel. */
float mh3_scene_glare_brightness(const mh3_scene_glare_plan *plan,const float rgb[3]);
/* CPU resource/menu dependencies retain their original guest addresses. The
 * boundary supplies typed memory and ordinary CPU calls; no graphics commands
 * or framebuffer access are accepted by this workflow. */
typedef struct mh3_scene_cpu_ops {
    void *context;
    uint32_t (*read)(void *,uint32_t,unsigned);
    void (*write)(void *,uint32_t,uint32_t,unsigned);
    uint32_t (*call)(void *,uint32_t,const uint32_t *,unsigned);
    void (*copy)(void *,uint32_t,uint32_t,uint32_t);
    void (*zero)(void *,uint32_t,uint32_t);
    void (*copy_string)(void *,uint32_t,uint32_t);
    int (*compare_string)(void *,uint32_t,uint32_t);
    void (*texture_object)(void *,uint32_t,uint32_t);
    void (*begin_shape_sample)(void *,int16_t,int16_t);
    int (*sample_shape)(void *,int16_t,int16_t,uint8_t[4]);
    float (*float_call)(void *,uint32_t,const uint32_t *,unsigned,const float *,unsigned);
} mh3_scene_cpu_ops;
int mh3_scene_equipment_update(const mh3_scene_cpu_ops *ops,uint32_t owner,uint32_t work,uint32_t slot,uint32_t scratch);
int mh3_scene_inner_color_menu(const mh3_scene_cpu_ops *ops,uint32_t self,uint32_t scratch);
int mh3_scene_equipment_color_menu(const mh3_scene_cpu_ops *ops,uint32_t self,uint32_t scratch);
int mh3_scene_demo_character_menu(const mh3_scene_cpu_ops *ops,uint32_t scratch);
int mh3_scene_demo_setup(float width,float height,const mh3_draw_state *draw,
                         const mh3_native_material_state *material,mh3_scene_quad_plan *out);
void mh3_scene_demo_quad(mh3_scene_quad_plan *plan,int16_t x,int16_t y,int16_t width,int16_t height,
                         const float uv0[2],const float uv1[2],const uint8_t rgba[4],int additive);
/* Sample authored texture bytes, never GPU pixels. -1 invalid,0 outside,
 * 1 covered. Output RGBA floats include interpolated vertex modulation. */
int mh3_scene_source_quad(mh3_res_span pixels,mh3_res_span palette,uint32_t palette_format,
    const mh3_native_texture_binding *binding,const AuroraNativeVertex vertices[4],
    const mh3_draw_state *state,const float point[2],float out[4]);
#ifdef __cplusplus
}
#endif
#endif
