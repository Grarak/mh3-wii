#ifndef MH3_LIGHTING_H
#define MH3_LIGHTING_H

#include "mh3_model_matrices.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MH3_LIGHT_SPOT = 1,
    MH3_LIGHT_SPECULAR = 2,
    MH3_LIGHT_ENABLED = 4,
    MH3_LIGHT_SPECULAR_DIRECTION = 8,
    MH3_LIGHT_DISABLE_COLOR = 16,
    MH3_LIGHT_DISABLE_ALPHA = 32,
    MH3_LIGHT_MAX = 128,
    MH3_LIGHT_SET_MAX = 128,
    MH3_LIGHT_SLOTS = 8,
    MH3_LIGHT_RECORD_BYTES = 68,
    MH3_LIGHT_SET_RECORD_BYTES = 12
};

typedef struct {
    uint8_t r, g, b, a;
} mh3_rgba8;
typedef struct {
    uint32_t flags;
    mh3_rgba8 color;
    float angular[3], distance[3];
    mh3_vec3 position;
    /* Vector consumed by lighting: opposite the ray direction passed to
     * set_direction(). Specular view preparation replaces it by a half vector. */
    mh3_vec3 direction;
} mh3_light;
typedef struct {
    int8_t light[8], ambient;
} mh3_light_set;

void mh3_light_default(size_t index, mh3_light *out);
void mh3_light_set_position(mh3_light *light, float x, float y, float z);
void mh3_light_set_direction(mh3_light *light, float x, float y, float z, int specular);
void mh3_light_set_angular(mh3_light *light, float a, float b, float c);
void mh3_light_set_distance(mh3_light *light, float a, float b, float c);
void mh3_light_set_spot(mh3_light *light, float cutoff, uint32_t shape);
void mh3_light_set_distance_attenuation(mh3_light *light, float distance, float brightness, uint32_t shape);
void mh3_light_set_shininess(mh3_light *light, float shininess);
void mh3_light_apply_view(mh3_light *light, const mh3_matrix34 *view);
mh3_vec3 mh3_light_ray_direction(const mh3_light *light);

/* CPU compatibility records remain big endian while translated callers use
 * them. Native lighting/shader inputs above are ordinary float records.
 * Store preserves the three reserved words in each serialized light object. */
mh3_res_status mh3_light_read(mh3_res_span record, mh3_light *out);
mh3_res_status mh3_light_store(mh3_res_mut_span record, const mh3_light *light);
mh3_res_status mh3_light_store_vector(mh3_res_mut_span record, const mh3_vec3 *vector);
mh3_res_status mh3_light_clear_record(mh3_res_mut_span record);
mh3_res_status mh3_light_copy_record(mh3_res_mut_span destination, mh3_res_span source);
mh3_res_status mh3_light_setting_store_defaults(mh3_res_mut_span descriptor, mh3_res_mut_span lights, mh3_res_mut_span ambient, mh3_res_mut_span sets, uint16_t light_count, uint16_t set_count,
                                                uint32_t light_address, uint32_t ambient_address, uint32_t set_address);

#ifdef __cplusplus
}
#endif
#endif
