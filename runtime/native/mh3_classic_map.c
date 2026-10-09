#include "mh3_classic_map.h"

#include <math.h>

/* Vita sticks rest within about 10 of 128 and reach 0/255 along the axes. */
const mh3_stick_curve mh3_vita_stick_curve = {0.12f, 0.90f, 0.80f};
/* SDL gamepads (Xbox, DualShock/DualSense, Switch Pro): larger sticks, more
 * centre drift on worn ones. */
const mh3_stick_curve mh3_gamepad_stick_curve = {0.15f, 0.95f, 0.80f};

static float clampf(float value, float low, float high) {
    return value < low ? low : value > high ? high : value;
}

static int16_t report_axis(float post_play) {
    /* Add WPADClampStick's play back so the game sees `post_play`. */
    const long rounded = lroundf(post_play);

    if (rounded == 0)
        return 0;

    const long raw = rounded > 0 ? rounded + MH3_CL_STICK_PLAY : rounded - MH3_CL_STICK_PLAY;
    return (int16_t)(raw > 511 ? 511 : raw < -512 ? -512 : raw);
}

void mh3_classic_stick(float x, float y, const mh3_stick_curve *curve, int16_t *out_x, int16_t *out_y) {
    x = clampf(x, -1.0f, 1.0f);
    y = clampf(y, -1.0f, 1.0f);
    const float length = sqrtf(x * x + y * y);

    if (!(length > curve->dead_zone) || !(curve->saturation > curve->dead_zone)) {
        *out_x = *out_y = 0;
        return;
    }

    const float t = clampf((length - curve->dead_zone) / (curve->saturation - curve->dead_zone), 0.0f, 1.0f);
    /* One above the game's threshold: rounding each axis must not drop the
     * clamped length below 30 at the start of the window. */
    const float first = MH3_CL_GAME_DEAD + 1.0f, full = MH3_CL_GAME_FULL;
    const float reach = 512.0f - MH3_CL_STICK_PLAY;
    const float window = clampf(curve->window, 0.05f, 1.0f);
    float target;

    if (t <= window)
        target = first + (full - first) * (t / window);
    else
        target = full + (reach - full) * ((t - window) / (1.0f - window));

    *out_x = report_axis(x / length * target);
    *out_y = report_axis(y / length * target);
}

void mh3_classic_clamp(int16_t x, int16_t y, int *out_x, int *out_y) {
    /* WPADClampStick, circle (0x804F9280): per-axis play, then the radius. */
    int cx = x, cy = y;
    cx = (-MH3_CL_STICK_PLAY < cx && cx < MH3_CL_STICK_PLAY) ? 0 : cx > 0 ? cx - MH3_CL_STICK_PLAY : cx + MH3_CL_STICK_PLAY;
    cy = (-MH3_CL_STICK_PLAY < cy && cy < MH3_CL_STICK_PLAY) ? 0 : cy > 0 ? cy - MH3_CL_STICK_PLAY : cy + MH3_CL_STICK_PLAY;
    const int squared = cx * cx + cy * cy;

    if (MH3_CL_STICK_RADIUS * MH3_CL_STICK_RADIUS < squared) {
        const int length = (int)sqrtf((float)squared);
        cx = cx * MH3_CL_STICK_RADIUS / length;
        cy = cy * MH3_CL_STICK_RADIUS / length;
    }

    *out_x = cx;
    *out_y = cy;
}

void mh3_classic_read_sticks(const int16_t clamped[4], mh3_classic_sticks *out) {
    /* 22.5 degree sectors from +x (0x805816E0). */
    static const uint8_t sectors[16] = {8, 9, 9, 1, 1, 5, 5, 4, 4, 6, 6, 2, 2, 10, 10, 8};
    const float turn = 6.28318548f; /* sda2 0x80797FC8 */
    for (int i = 0; i < 2; ++i) {
        const int x = clamped[2 * i], y = clamped[2 * i + 1];
        /* The guest's single-precision steps: atan2 (y, x), frsp, a full
         * turn added below 0, then degrees truncated. */
        float angle = 0.0f;

        if (x || y) {
            angle = (float)atan2((double)y, (double)x);

            if (angle < 0.0f)
                angle = angle + turn;
        }

        uint16_t degrees = (uint16_t)(int)(360.0f * (angle / turn));
        uint16_t length = (uint16_t)(int)sqrt((double)(y * y + x * x));

        if (length > MH3_CL_GAME_FULL)
            length = MH3_CL_GAME_FULL;

        if (length < MH3_CL_GAME_DEAD)
            degrees = length = 0;

        out->degrees[i] = degrees;
        out->length[i] = length;
        out->turn[i] = (uint16_t)(((int32_t)degrees << 16) / 360);
        out->direction[i] = length < 37 ? 0 : sectors[(int)((float)degrees / 22.5f) & 15];
    }

    static const uint16_t left[4] = {0x2000, 0x1000, 0x800, 0x400}, right[4] = {0x20, 0x10, 0x8, 0x4};
    out->buttons = 0;

    for (int bit = 0; bit < 4; ++bit) {
        if (out->direction[0] & (1 << bit))
            out->buttons |= left[bit];

        if (out->direction[1] & (1 << bit))
            out->buttons |= right[bit];
    }
}

int mh3_classic_game_length(int16_t x, int16_t y) {
    int cx, cy;
    mh3_classic_clamp(x, y, &cx, &cy);
    const int16_t clamped[4] = {(int16_t)cx, (int16_t)cy, 0, 0};
    mh3_classic_sticks sticks;
    mh3_classic_read_sticks(clamped, &sticks);
    return sticks.length[0];
}

static float vita_axis(uint8_t value) {
    return clampf(((float)value - 128.0f) / 127.0f, -1.0f, 1.0f);
}

void mh3_vita_pad_to_classic(const mh3_vita_pad *pad, mh3_classic_state *out) {
    static const struct {
        uint32_t vita;
        uint16_t classic;
    } buttons[] = {
        {MH3_VITA_CIRCLE, MH3_CL_A},  {MH3_VITA_CROSS, MH3_CL_B},     {MH3_VITA_TRIANGLE, MH3_CL_X}, {MH3_VITA_SQUARE, MH3_CL_Y},
        {MH3_VITA_UP, MH3_CL_UP},     {MH3_VITA_DOWN, MH3_CL_DOWN},   {MH3_VITA_LEFT, MH3_CL_LEFT},  {MH3_VITA_RIGHT, MH3_CL_RIGHT},
        {MH3_VITA_L1, MH3_CL_L},      {MH3_VITA_R1, MH3_CL_R},        {MH3_VITA_L2, MH3_CL_ZL},      {MH3_VITA_R2, MH3_CL_ZR},
        {MH3_VITA_START, MH3_CL_PLUS}, {MH3_VITA_SELECT, MH3_CL_MINUS},
    };

    mh3_classic_state state = {0};

    for (unsigned i = 0; i < sizeof buttons / sizeof buttons[0]; ++i)
        if (pad->buttons & buttons[i].vita)
            state.buttons |= buttons[i].classic;
    if (pad->lt > 128)
        state.buttons |= MH3_CL_ZL;

    if (pad->rt > 128)
        state.buttons |= MH3_CL_ZR;

    if ((pad->buttons & (MH3_VITA_L3 | MH3_VITA_R3)) == (MH3_VITA_L3 | MH3_VITA_R3))
        state.buttons |= MH3_CL_HOME;
    /* Rear panel: the upper quarters, under the index fingers that leave L/R.
     * The lower half is where a gripping hand rests. */
    for (unsigned i = 0; i < pad->rear_count && i < MH3_VITA_MAX_TOUCH; ++i)
        if (pad->rear[i].y < 0.5f)
            state.buttons |= pad->rear[i].x < 0.5f ? MH3_CL_ZL : MH3_CL_ZR;
    /* Front panel: HOME on a touch at the top centre of the screen. */
    for (unsigned i = 0; i < pad->front_count && i < MH3_VITA_MAX_TOUCH; ++i)
        if (pad->front[i].y < 0.15f && pad->front[i].x > 0.35f && pad->front[i].x < 0.65f)
            state.buttons |= MH3_CL_HOME;
    /* The handheld's L/R are digital: a fully pressed analog trigger. */
    state.trigger_l = (state.buttons & MH3_CL_L) ? 255 : 0;
    state.trigger_r = (state.buttons & MH3_CL_R) ? 255 : 0;
    mh3_classic_stick(vita_axis(pad->lx), -vita_axis(pad->ly), &mh3_vita_stick_curve, &state.lx, &state.ly);
    mh3_classic_stick(vita_axis(pad->rx), -vita_axis(pad->ry), &mh3_vita_stick_curve, &state.rx, &state.ry);
    *out = state;
}
