#include "mh3_model_cull.h"
#include <math.h>

static int finite(float v) {
    return __builtin_isfinite(v);
}

int mh3_cull_frustum_make(const float p[16], const float lo[2], const float hi[2], mh3_cull_frustum *out) {
    if (!p || !lo || !hi || !out)
        return 0;

    for (unsigned i = 0; i < 16; i++)
        if (!finite(p[i]))
            return 0;
    for (unsigned i = 0; i < 2; i++)
        if (!finite(lo[i]) || !finite(hi[i]) || !(lo[i] < hi[i]))
            return 0;
    /* Row r of P gives clip coordinate r. x >= lo*w: P0 - lo*P3 >= 0;
     * x <= hi*w: hi*P3 - P0 >= 0; the same for y with P1. */
    for (unsigned axis = 0; axis < 2; axis++)
        for (unsigned c = 0; c < 4; c++) {
            const float a = p[axis * 4 + c], w = p[12 + c];
            out->plane[axis * 2][c] = a - lo[axis] * w;
            out->plane[axis * 2 + 1][c] = hi[axis] * w - a;
        }
    return 1;
}

int mh3_cull_scissor_rect(const float viewport[4], const int32_t scissor[4], float margin, float lo[2], float hi[2]) {
    if (!viewport || !scissor || !lo || !hi)
        return 0;

    const float vx = viewport[0], vy = viewport[1], vw = viewport[2], vh = viewport[3];

    if (!finite(vx) || !finite(vy) || !finite(vw) || !finite(vh) || !(vw > 0) || !(vh > 0) || !finite(margin) || margin < 0)
        return 0;

    const float left = (float)scissor[0], top = (float)scissor[1];
    const float right = left + (float)scissor[2], bottom = top + (float)scissor[3];
    /* x grows right, logical y grows down; Wii NDC y grows up. */
    lo[0] = 2.f * (left - vx) / vw - 1.f - margin;
    hi[0] = 2.f * (right - vx) / vw - 1.f + margin;
    lo[1] = 1.f - 2.f * (bottom - vy) / vh - margin;
    hi[1] = 1.f - 2.f * (top - vy) / vh + margin;
    return finite(lo[0]) && finite(hi[0]) && finite(lo[1]) && finite(hi[1]);
}

/* The largest value of the view-space plane over the box transformed by m. */
static float box_plane_max(const float plane[4], const float m[12], const mh3_cull_box *box) {
    float n[3], d = plane[3];

    for (unsigned c = 0; c < 3; c++)
        n[c] = plane[0] * m[c] + plane[1] * m[4 + c] + plane[2] * m[8 + c];

    d += plane[0] * m[3] + plane[1] * m[7] + plane[2] * m[11];
    float value = d;

    for (unsigned c = 0; c < 3; c++) {
        const float center = 0.5f * (box->min[c] + box->max[c]), extent = 0.5f * (box->max[c] - box->min[c]);
        value += n[c] * center + fabsf(n[c]) * extent;
    }

    return value;
}

int mh3_cull_boxes_outside(const mh3_cull_frustum *f, const float *const *matrices, const mh3_cull_box *boxes, unsigned count) {
    if (!f || !matrices || !boxes || !count)
        return 0;

    for (unsigned k = 0; k < 4; k++) {
        unsigned i = 0;

        for (; i < count; i++) {
            const float value = box_plane_max(f->plane[k], matrices[i], &boxes[i]);
            /* NaN compares false: never culled. */
            if (!(value < 0.f))
                break;
        }

        if (i == count)
            return 1;
    }

    return 0;
}
