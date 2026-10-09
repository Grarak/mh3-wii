#ifndef MH3_PREPARE_SKIN_H
#define MH3_PREPARE_SKIN_H
#include "mh3_model_resources.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* GPU skinning tables of one model (docs/gpu-skinning.md), from its NodeMix
 * bytecode. Offline preparation only: no allocation, no live pose data. */
enum {
    MH3_SKIN_INFLUENCES = 4,
    MH3_SKIN_MAX_ROWS = 255,
    MH3_SKIN_MAX_MATRICES = 2048
};

/* One draw matrix ID. An envelope (NodeMix op 3) names up to four skin bones
 * and weights, the largest four renormalized when it had more; count 0 is a
 * node's own matrix. bone marks a skin bone (op 5): its inverse bind matrix
 * (the node's, row-major 3x4) turns its world matrix into its skinning matrix. */
typedef struct {
    uint16_t source[MH3_SKIN_INFLUENCES];
    float weight[MH3_SKIN_INFLUENCES];
    uint8_t count, bone;
    float inverse_bind[12];
} mh3_skin_matrix;

typedef struct {
    const mh3_skin_matrix *matrices;
    uint32_t count;
} mh3_skin_model;

typedef enum {
    MH3_SKIN_OK = 0,
    MH3_SKIN_INVALID = -1,
    MH3_SKIN_RANGE = -2,
    MH3_SKIN_UNSUPPORTED = -3
} mh3_skin_status;

/* Fills storage[0, matrices) for the model's draw matrices (its matrix-to-node
 * map's count). A model without NodeMix has only node matrices. Fails when the
 * model has more than capacity matrices or malformed NodeMix commands. */
mh3_skin_status mh3_prepare_skin_model(const mh3_res_model *model, mh3_skin_matrix *storage, uint32_t capacity, mh3_skin_model *out);
#ifdef __cplusplus
}
#endif
#endif
