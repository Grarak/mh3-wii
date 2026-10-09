#ifndef MH3_PARTICLE_DRAW_STATE_H
#define MH3_PARTICLE_DRAW_STATE_H
#include "mh3_effect_draw.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Captured host values. No guest addresses, callbacks, or resource pointers.
 * ParticleRotation/direction selection and authored table samples must be
 * resolved by the CPU capture adapter before passing this record to a renderer. */
typedef struct mh3_particle_directional_scene {
    float view_local[12],fallback[3],pivot[2],movement_scale;
    mh3_effect_quad canonical[2];
    uint8_t screen_facing,plane_count,rotation_kind,size_kind,stretch;
} mh3_particle_directional_scene;
typedef struct mh3_particle_directional_record {
    float position[3],direction[3],movement[3],width,height;
    /* sine/cosine of NEGATIVE resolved particle rotation, by X/Y/Z axis.
     * An unused or zero-angle axis supplies {0,1}. */
    float sincos[3][2];
} mh3_particle_directional_record;
typedef struct mh3_particle_directional_output {
    float history[3],matrix[12];
    mh3_effect_quad quads[2];
    uint8_t plane_count;
} mh3_particle_directional_output;

/* Native C counterparts of 800BCEFC and 800BCD78. Nonzero size_kind retains
 * the authored column reorder and asymmetric Y/Z translation. */
void mh3_particle_directional_rotation(uint8_t kind,const float sincos[3][2],float out[12]);
void mh3_particle_directional_size(const float pivot[2],float width,float height,float stretch,
                                   uint8_t kind,const float rotation[12],float out[12]);
/* history is render-owned storage, supplied separately from immutable capture.
 * Success updates only output; callers choose where to publish output.history.
 * Invalid arguments leave output untouched. This does not initialize the
 * manager's sentinel suffix: that traversal precedes visible-particle drawing. */
mh3_res_status mh3_particle_directional_draw(const mh3_particle_directional_scene *scene,
                                            const mh3_particle_directional_record *particle,
                                            const float history[3],mh3_particle_directional_output *out);
#ifdef __cplusplus
}
#endif
#endif
