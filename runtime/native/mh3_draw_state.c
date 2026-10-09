#include "mh3_draw_state.h"
#include <math.h>
#include <string.h>

void mh3_draw_identity_position(float out[12]) {
    static const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    memcpy(out, identity, sizeof(identity));
}

int mh3_draw_ortho(float out[16], float top, float bottom, float left, float right, float near_z, float far_z) {
    if (!out || !isfinite(top) || !isfinite(bottom) || !isfinite(left) || !isfinite(right) || !isfinite(near_z) || !isfinite(far_z) || top == bottom || left == right || near_z == far_z)
        return 0;

    const float dx = 1.f / (right - left), dy = 1.f / (top - bottom), dz = 1.f / (far_z - near_z);
    const float matrix[16] = {2.f * dx, 0, 0, -(right + left) * dx, 0, 2.f * dy, 0, -(top + bottom) * dy, 0, 0, -dz, -far_z * dz, 0, 0, 0, 1};
    memcpy(out, matrix, sizeof(matrix));
    return 1;
}

void mh3_draw_vertex(const mh3_draw_state *state, const float position[3], const float uv[2], const uint8_t color[4], AuroraNativeVertex *out) {
    float view[4], clip[4];

    for (unsigned row = 0; row < 3; ++row) {
        const float *m = state->position + row * 4;
        view[row] = ((m[0] * position[0] + m[1] * position[1]) + m[2] * position[2]) + m[3];
    }

    view[3] = 1.f;

    for (unsigned row = 0; row < 4; ++row) {
        const float *m = state->projection + row * 4;
        clip[row] = ((m[0] * view[0] + m[1] * view[1]) + m[2] * view[2]) + m[3] * view[3];
    }

    // A reversed viewport already reverses the camera's near/far ordering.
    const float depth = state->viewport[4] <= state->viewport[5] ? -clip[2] : clip[2] + clip[3];
    out->position[0] = clip[0];
    out->position[1] = clip[1];
    out->position[2] = 2.f * depth - clip[3];
    out->position[3] = clip[3];
    out->uv[0] = uv ? uv[0] : 0;
    out->uv[1] = uv ? uv[1] : 0;
    memcpy(out->color, color, 4);
}
