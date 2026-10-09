#ifndef MH3_TEXTURE_MATRICES_H
#define MH3_TEXTURE_MATRICES_H
#include "mh3_model_state.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 800871C0 ResTexSrt: the same typed slots exported by M3MT. Only XSI
 * matrix mode is enabled in MH3's 80075F00; other modes are unsupported. */
typedef struct {
    uint32_t flags,mode;
    mh3_pack_texture_srt slot[8];
} mh3_native_texture_srt;
mh3_res_status mh3_texture_srt_read(mh3_res_span bytes,mh3_native_texture_srt *out);
/* 80075F00/800D754C. flag is the source nibble (exists,scale-one,
 * rotation-zero,translation-zero). set replaces the matrix; otherwise the
 * original specialized XSI product formulas update its first two rows. */
mh3_res_status mh3_texture_srt_calc(const mh3_pack_texture_srt *srt,uint32_t mode,
                                   int set,mh3_matrix34 *matrix);

/* Inputs of the built-in mapping functions 80084694/80084A78/80084C00.
 * Light records already have the G3D view transform applied by their producer.
 * Camera-reference precedence differs between environment and specular maps. */
typedef struct {
    mh3_matrix34 current,inverse_current,environment,projection,referenced;
    mh3_light light;
    int reference_present,light_present;
} mh3_texture_mapping_input;
mh3_res_status mh3_texture_mapping(uint32_t map_mode,
                                  const mh3_texture_mapping_input *input,
                                  mh3_matrix34 *out);
/* Checked selection from serialized G3D CameraState and LightState. Changes
 * the inverse-cache bytes only after successful complete extraction. */
mh3_res_status mh3_texture_mapping_read(mh3_res_mut_span camera_state,
                                       mh3_res_span light_state,
                                       uint32_t map_mode,
                                       int32_t camera_reference,int32_t light_reference,
                                       mh3_texture_mapping_input *out);

typedef struct {
    mh3_matrix34 matrix[8];
    uint32_t source[8]; /* 0 coordinates/identity,1 normal,2 position */
    uint8_t present,identity,apply;
} mh3_texture_matrices;
/* supplied_mapping is precomputed for every present nonzero-map slot. Output
 * and next identity mask remain unchanged when any input is unsupported. */
mh3_res_status mh3_texture_matrices_prepare(const mh3_native_texture_srt *srt,
        const mh3_matrix34 supplied_mapping[8],const uint32_t mapping_source[8],
        uint32_t previous_identity,mh3_texture_matrices *out,
        uint32_t *next_identity);
/* 8009B330: copy a normal matrix into 3x4 texture space, with zero translation. */
mh3_res_status mh3_texture_matrix_from_normal(const mh3_matrix33 *normal,
                                              mh3_matrix34 *out);
#ifdef __cplusplus
}
#endif
#endif
