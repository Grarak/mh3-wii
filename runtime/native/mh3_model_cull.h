#ifndef MH3_MODEL_CULL_H
#define MH3_MODEL_CULL_H
/* Conservative visibility of native model draws.
 *
 * Clip coordinates follow the native mesh shaders: a vertex v selects a
 * palette position matrix M (3x4 row-major, object to view space) and
 * c = P * (M * v) with P the Wii-convention row-major projection. A primitive
 * can produce pixels only where x/w and y/w (Wii orientation, +y up) lie in
 * the drawable rectangle of the pass; w > 0 follows from the two x (or y)
 * planes. Near and far planes are not tested: the backends clip depth
 * differently.
 *
 * A draw is culled only when one plane rejects every box: then the convex hull
 * of all its vertices, and therefore every triangle, lies outside. Boxes are in
 * the vertex position space (the converted mesh's position attribute). */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float min[3], max[3];
} mh3_cull_box;
/* View-space half spaces: inside where p[0]x + p[1]y + p[2]z + p[3] >= 0. */
typedef struct {
    float plane[4][4];
} mh3_cull_frustum;

/* The planes of lo[i] <= ndc[i] <= hi[i] (i = x, y; Wii orientation) for the
 * projection. Returns 0 (no frustum: do not cull) when the rectangle is empty
 * or inverted or any input is not finite. */
int mh3_cull_frustum_make(const float projection[16], const float lo[2], const float hi[2], mh3_cull_frustum *out);
/* The drawable NDC rectangle of a pass: the scissor rectangle (logical units,
 * x, y, width, height; y down) in the coordinates of the viewport (x, y,
 * width, height), widened by margin on every side. Returns 0 when the viewport
 * is degenerate. GL clips to the viewport as well, GXM only to the scissor
 * region; the scissor rectangle bounds both. */
int mh3_cull_scissor_rect(const float viewport[4], const int32_t scissor[4], float margin, float lo[2], float hi[2]);
/* 1 when one plane of the frustum rejects box i transformed by matrices[i]
 * (12 floats, 3x4 row-major) for every i < count; 0 otherwise, also for
 * count 0 or non-finite values. */
int mh3_cull_boxes_outside(const mh3_cull_frustum *frustum, const float *const *matrices, const mh3_cull_box *boxes, unsigned count);

#ifdef __cplusplus
}
#endif
#endif
