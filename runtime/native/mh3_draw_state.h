#ifndef MH3_DRAW_STATE_H
#define MH3_DRAW_STATE_H
#include <stdint.h>
#include <aurora/native_geometry.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Typed application raster state. Comparisons: never,less,equal,lequal,
 * greater,not-equal,gequal,always. Factors: zero,one,src-color,inv-src-color,
 * src-alpha,inv-src-alpha,dst-alpha,inv-dst-alpha. Blend modes: off,blend,
 * logic,subtract. Cull: none,front,back,both. These are semantic values;
 * this representation contains no packed hardware register or FIFO state. */
typedef struct mh3_raster_state {
    uint32_t blend_mode, src_factor, dst_factor, logic_op;
    uint32_t depth_test, depth_compare, depth_write, cull_mode;
    uint32_t color_write, alpha_write;
} mh3_raster_state;
typedef struct mh3_draw_state {
    float projection[16], position[12], viewport[6];
    uint32_t perspective;
    mh3_raster_state raster;
} mh3_draw_state;

void mh3_draw_identity_position(float out[12]);
/* Wii camera convention, row-major: near maps to -w, far to zero. */
int mh3_draw_ortho(float out[16],float top,float bottom,float left,float right,float near_z,float far_z);
/* Writes homogeneous GL clip coordinates in reversed depth, Y up. */
void mh3_draw_vertex(const mh3_draw_state *state,const float position[3],
                     const float uv[2],const uint8_t color[4],AuroraNativeVertex *out);
#ifdef __cplusplus
}
#endif
#endif
