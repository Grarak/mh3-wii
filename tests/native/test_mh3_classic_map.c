/* Vita pad and host stick mapping to the Classic Controller report. */
#include "runtime/native/mh3_classic_map.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition, ...)                                                                                          \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            ++failures;                                                                                                \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);                                                            \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

static mh3_vita_pad centred(void) {
    mh3_vita_pad pad;
    memset(&pad, 0, sizeof pad);
    pad.lx = pad.ly = pad.rx = pad.ry = 128;
    return pad;
}

static mh3_classic_state map(const mh3_vita_pad *pad) {
    mh3_classic_state state;
    mh3_vita_pad_to_classic(pad, &state);
    return state;
}

static void buttons(void) {
    static const struct {
        uint32_t vita;
        uint16_t classic;
    } cases[] = {
        {MH3_VITA_CIRCLE, MH3_CL_A},  {MH3_VITA_CROSS, MH3_CL_B},       {MH3_VITA_TRIANGLE, MH3_CL_X}, {MH3_VITA_SQUARE, MH3_CL_Y},
        {MH3_VITA_UP, MH3_CL_UP},     {MH3_VITA_DOWN, MH3_CL_DOWN},     {MH3_VITA_LEFT, MH3_CL_LEFT},  {MH3_VITA_RIGHT, MH3_CL_RIGHT},
        {MH3_VITA_L1, MH3_CL_L},      {MH3_VITA_R1, MH3_CL_R},          {MH3_VITA_L2, MH3_CL_ZL},      {MH3_VITA_R2, MH3_CL_ZR},
        {MH3_VITA_START, MH3_CL_PLUS}, {MH3_VITA_SELECT, MH3_CL_MINUS}, {MH3_VITA_L3, 0},              {MH3_VITA_R3, 0},
    };

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        mh3_vita_pad pad = centred();
        pad.buttons = cases[i].vita;
        const mh3_classic_state state = map(&pad);
        CHECK(state.buttons == cases[i].classic, "Vita 0x%x gave Classic 0x%x, expected 0x%x", cases[i].vita, state.buttons, cases[i].classic);
        CHECK(state.lx == 0 && state.ly == 0 && state.rx == 0 && state.ry == 0, "buttons moved a stick");
    }

    mh3_vita_pad pad = centred();
    CHECK(map(&pad).buttons == 0 && map(&pad).trigger_l == 0 && map(&pad).trigger_r == 0, "idle pad pressed something");
    pad.buttons = MH3_VITA_L1;
    CHECK(map(&pad).trigger_l == 255 && map(&pad).trigger_r == 0, "L is not a fully pressed L trigger");
    pad.buttons = MH3_VITA_R1;
    CHECK(map(&pad).trigger_r == 255 && map(&pad).trigger_l == 0, "R is not a fully pressed R trigger");
    pad.buttons = MH3_VITA_L3 | MH3_VITA_R3;
    CHECK(map(&pad).buttons == MH3_CL_HOME, "L3+R3 is not HOME");
    pad.buttons = 0;
    pad.lt = 200;
    pad.rt = 100;
    CHECK(map(&pad).buttons == MH3_CL_ZL, "PS TV analog L2/R2 threshold");
}

static void touch(void) {
    static const struct {
        float x, y;
        uint16_t classic;
    } rear[] = {
        {0.1f, 0.1f, MH3_CL_ZL}, {0.45f, 0.45f, MH3_CL_ZL}, {0.55f, 0.2f, MH3_CL_ZR}, {0.9f, 0.05f, MH3_CL_ZR},
        {0.1f, 0.6f, 0},         {0.9f, 0.95f, 0},          {0.5f, 0.75f, 0},
    };

    for (unsigned i = 0; i < sizeof rear / sizeof rear[0]; ++i) {
        mh3_vita_pad pad = centred();
        pad.rear_count = 1;
        pad.rear[0] = (mh3_touch_point){rear[i].x, rear[i].y};
        CHECK(map(&pad).buttons == rear[i].classic, "rear touch %.2f,%.2f gave 0x%x", rear[i].x, rear[i].y, map(&pad).buttons);
    }

    mh3_vita_pad pad = centred();
    pad.rear_count = 2;
    pad.rear[0] = (mh3_touch_point){0.2f, 0.3f};
    pad.rear[1] = (mh3_touch_point){0.8f, 0.3f};
    CHECK(map(&pad).buttons == (MH3_CL_ZL | MH3_CL_ZR), "both rear quarters");
    static const struct {
        float x, y;
        uint16_t classic;
    } front[] = {{0.5f, 0.05f, MH3_CL_HOME}, {0.4f, 0.1f, MH3_CL_HOME}, {0.5f, 0.3f, 0}, {0.1f, 0.05f, 0}, {0.9f, 0.05f, 0}, {0.5f, 0.9f, 0}};

    for (unsigned i = 0; i < sizeof front / sizeof front[0]; ++i) {
        pad = centred();
        pad.front_count = 1;
        pad.front[0] = (mh3_touch_point){front[i].x, front[i].y};
        CHECK(map(&pad).buttons == front[i].classic, "front touch %.2f,%.2f gave 0x%x", front[i].x, front[i].y, map(&pad).buttons);
    }
}

static void sticks(void) {
    /* Centre and drift within the dead zone report nothing. */
    for (int dx = -10; dx <= 10; dx += 5)
        for (int dy = -10; dy <= 10; dy += 5) {
            mh3_vita_pad pad = centred();
            pad.lx = (uint8_t)(128 + dx);
            pad.ly = (uint8_t)(128 + dy);
            pad.rx = (uint8_t)(128 - dy);
            pad.ry = (uint8_t)(128 + dx);
            const mh3_classic_state state = map(&pad);
            CHECK(state.lx == 0 && state.ly == 0 && state.rx == 0 && state.ry == 0, "drift %d,%d reported %d,%d", dx, dy, state.lx, state.ly);
        }
    /* Full deflection: the whole report range, y up, as the recorded logs
     * and the devkit harness expect (|axis| > 400). */
    mh3_vita_pad pad = centred();
    pad.ly = 0;
    pad.rx = 255;
    mh3_classic_state state = map(&pad);
    CHECK(state.lx == 0 && state.ly == 511 && state.rx == 511 && state.ry == 0, "full up/right gave %d,%d %d,%d", state.lx, state.ly, state.rx, state.ry);
    pad.ly = 255;
    pad.rx = 0;
    state = map(&pad);
    CHECK(state.ly <= -511 && state.rx <= -511, "full down/left gave %d %d", state.ly, state.rx);
    CHECK(mh3_classic_game_length(state.lx, state.ly) == MH3_CL_GAME_FULL, "full deflection is not the game's full length");

    /* Along each direction the game's length starts at its threshold just
     * past the dead zone, grows monotonically and is full before the edge. */
    for (int degrees = 0; degrees < 360; degrees += 5) {
        const float angle = (float)degrees * 3.14159265f / 180.0f;
        int previous = 0, first = -1, full = -1;

        for (int step = 0; step <= 100; ++step) {
            const float r = (float)step / 100.0f;
            int16_t x, y;
            mh3_classic_stick(r * cosf(angle), r * sinf(angle), &mh3_vita_stick_curve, &x, &y);
            const int length = mh3_classic_game_length(x, y);
            CHECK(length >= previous, "length fell at %d degrees, r %.2f", degrees, r);
            previous = length;

            if (length && first < 0) {
                first = step;
                CHECK(length >= MH3_CL_GAME_DEAD && length <= MH3_CL_GAME_DEAD + 4, "first length %d at %d degrees", length, degrees);
            }

            if (length == MH3_CL_GAME_FULL && full < 0)
                full = step;

            if (length && length < MH3_CL_GAME_FULL) {
                /* The game sees the stick's direction. */
                int cx, cy;
                mh3_classic_clamp(x, y, &cx, &cy);
                float error = fabsf(atan2f((float)cy, (float)cx) - angle);

                if (error > 3.14159265f)
                    error = 2 * 3.14159265f - error;

                CHECK(error < 0.05f, "direction off by %.3f rad at %d degrees, r %.2f", error, degrees, r);
            }
        }

        CHECK(first >= 12 && first <= 14, "movement starts at r %d%% (%d degrees)", first, degrees);
        CHECK(full >= 70 && full <= 78, "full length at r %d%% (%d degrees)", full, degrees);
        /* Just past the dead zone: the game's threshold, not below it. */
        const float edge = mh3_vita_stick_curve.dead_zone + 1e-4f;
        int16_t x, y;
        mh3_classic_stick(edge * cosf(angle), edge * sinf(angle), &mh3_vita_stick_curve, &x, &y);
        const int start = mh3_classic_game_length(x, y);
        CHECK(start == MH3_CL_GAME_DEAD || start == MH3_CL_GAME_DEAD + 1, "length %d at the dead zone's edge, %d degrees", start, degrees);
    }

    /* The window is linear: half way through it, half the game's range. */
    const mh3_stick_curve *curve = &mh3_vita_stick_curve;
    const float middle = curve->dead_zone + (curve->saturation - curve->dead_zone) * curve->window * 0.5f;
    int16_t x, y;
    mh3_classic_stick(0.0f, middle, curve, &x, &y);
    const int length = mh3_classic_game_length(x, y);
    CHECK(abs(length - (MH3_CL_GAME_DEAD + 1 + MH3_CL_GAME_FULL) / 2) <= 1, "window middle length %d", length);

    /* The old linear mapping (host axis * 512) for comparison: it saturated
     * the game at about 37% of the Vita stick's travel. */
    int saturated = -1;

    for (int v = 0; v <= 127 && saturated < 0; ++v)
        if (mh3_classic_game_length(0, (int16_t)(v * 4)) == MH3_CL_GAME_FULL)
            saturated = v;
    CHECK(saturated > 40 && saturated < 50, "old mapping saturated at %d", saturated);

    /* Host gamepads: same window, their own dead zone. */
    mh3_classic_stick(0.14f, 0.0f, &mh3_gamepad_stick_curve, &x, &y);
    CHECK(x == 0 && y == 0, "gamepad dead zone");
    mh3_classic_stick(1.0f, 0.0f, &mh3_gamepad_stick_curve, &x, &y);
    CHECK(x == 511 && y == 0, "gamepad full right gave %d,%d", x, y);
}

static void clamp(void) {
    int x, y;
    mh3_classic_clamp(59, -59, &x, &y);
    CHECK(x == 0 && y == 0, "play");
    mh3_classic_clamp(100, -100, &x, &y);
    CHECK(x == 40 && y == -40, "play offset");
    mh3_classic_clamp(511, 0, &x, &y);
    CHECK(x == 248 && y == 0, "radius");
    CHECK(mh3_classic_game_length(89, 0) == 0 && mh3_classic_game_length(90, 0) == 30, "game threshold");
    CHECK(mh3_classic_game_length(187, 0) == 127 && mh3_classic_game_length(186, 0) == 126, "game saturation");
}

static void read_sticks(void) {
    /* Left stick up, right stick left: the game's +y is up. */
    const int16_t clamped[4] = {0, 100, -100, 0};
    mh3_classic_sticks sticks;
    mh3_classic_read_sticks(clamped, &sticks);
    CHECK(sticks.degrees[0] == 90 && sticks.turn[0] == 16384 && sticks.length[0] == 100 && sticks.direction[0] == 1, "left up: %u %u %u %u", sticks.degrees[0],
          sticks.turn[0], sticks.length[0], sticks.direction[0]);
    CHECK(sticks.degrees[1] == 180 && sticks.turn[1] == 32768 && sticks.length[1] == 100 && sticks.direction[1] == 4, "right left: %u %u %u %u", sticks.degrees[1],
          sticks.turn[1], sticks.length[1], sticks.direction[1]);
    CHECK(sticks.buttons == (0x2000 | 0x8), "stick buttons 0x%x", sticks.buttons);
    /* Short of 30 nothing; 30..36 a direction without 8-way bits; 248 is 127. */
    const int16_t short_and_mid[4] = {20, -20, 0, -33};
    mh3_classic_read_sticks(short_and_mid, &sticks);
    CHECK(sticks.length[0] == 0 && sticks.degrees[0] == 0 && sticks.turn[0] == 0, "below 30");
    CHECK(sticks.length[1] == 33 && sticks.degrees[1] == 270 && sticks.direction[1] == 0 && sticks.buttons == 0, "30..36");
    const int16_t diagonal[4] = {175, -175, 248, 0};
    mh3_classic_read_sticks(diagonal, &sticks);
    CHECK(sticks.length[0] == 127 && sticks.degrees[0] == 315 && sticks.direction[0] == (2 | 8), "down-right %u %u", sticks.degrees[0], sticks.direction[0]);
    CHECK(sticks.length[1] == 127 && sticks.degrees[1] == 0 && sticks.direction[1] == 8 && sticks.buttons == (0x1000 | 0x400 | 0x4), "right 0x%x", sticks.buttons);
}

int main(void) {
    buttons();
    touch();
    sticks();
    clamp();
    read_sticks();

    if (failures) {
        fprintf(stderr, "%d Classic mapping checks failed\n", failures);
        return 1;
    }

    puts("Classic Controller mapping checks passed");
    return 0;
}
