#ifndef MH3_CLASSIC_MAP_H
#define MH3_CLASSIC_MAP_H

/* Host controls to the Classic Controller (RVL-005) the game is given.
 *
 * The game reads the Classic report through WPADRead (it does not use KPAD
 * outside the HOME Menu): buttons through a fixed table into its own bits,
 * sticks through WPADClampStick (circle, 60 units of play per axis, radius
 * 248) and then its own stick reader (0x80042FDC), which takes the clamped
 * vector's length, ignores lengths below 30 and saturates at 127. Of a
 * stick's report range (-512..511) only the clamped lengths 30..127 move the
 * hunter or the camera by different amounts: raw 90..187 along an axis.
 *
 * mh3_classic_stick spreads a host stick's travel over that window: outside
 * the host dead zone the length starts at the game's threshold, grows linearly
 * to its saturation at `window` of the travel, then on to the full report
 * range (for KPAD readers such as the HOME Menu). The per-axis play is added
 * back so the game sees the intended direction. Pure functions, no host APIs. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WPADCLStatus button bits. */
enum {
    MH3_CL_UP = 0x0001,
    MH3_CL_LEFT = 0x0002,
    MH3_CL_ZR = 0x0004,
    MH3_CL_X = 0x0008,
    MH3_CL_A = 0x0010,
    MH3_CL_Y = 0x0020,
    MH3_CL_B = 0x0040,
    MH3_CL_ZL = 0x0080,
    MH3_CL_R = 0x0200, /* R trigger fully pressed */
    MH3_CL_PLUS = 0x0400,
    MH3_CL_HOME = 0x0800,
    MH3_CL_MINUS = 0x1000,
    MH3_CL_L = 0x2000, /* L trigger fully pressed */
    MH3_CL_DOWN = 0x4000,
    MH3_CL_RIGHT = 0x8000,
};

/* The report the game reads: sticks -512..511 with +y up, analog triggers 0..255. */
typedef struct {
    uint16_t buttons;
    int16_t lx, ly, rx, ry;
    uint8_t trigger_l, trigger_r;
} mh3_classic_state;

/* WPADClampStick's Classic circle (0x8079F900) and the game's stick reader. */
enum {
    MH3_CL_STICK_PLAY = 60,
    MH3_CL_STICK_RADIUS = 248,
    MH3_CL_GAME_DEAD = 30,
    MH3_CL_GAME_FULL = 127,
};

typedef struct {
    float dead_zone;  /* radial host deflection (0..1) read as centred */
    float saturation; /* radial host deflection read as full */
    float window;     /* share of the travel past the dead zone spent on the game's 30..127 */
} mh3_stick_curve;

/* The game's Classic stick reader (0x80042FDC) on WPADClampStick's output,
 * the fields it writes from the channel's pad record + 0xD6. Directions count
 * counter-clockwise from right (+x), with +y up. */
typedef struct {
    uint16_t buttons;     /* +0x08: the 8-way bits as buttons, left 0x2000..0x400, right 0x20..0x4 */
    uint16_t turn[2];     /* +0x10: direction in 65536ths of a turn */
    uint16_t length[2];   /* +0x14 and +0x1C: 0 below 30, at most 127 */
    uint16_t degrees[2];  /* +0x18: direction 0..359; 0 below length 30 */
    uint8_t direction[2]; /* +0x20: 8-way bits (1 up, 2 down, 4 left, 8 right); 0 below length 37 */
} mh3_classic_sticks;

/* clamped: left x, left y, right x, right y as WPADClampStick left them. */
void mh3_classic_read_sticks(const int16_t clamped[4], mh3_classic_sticks *out);

/* Host stick (x right, y up, each -1..1) to the Classic report's two axes. */
void mh3_classic_stick(float x, float y, const mh3_stick_curve *curve, int16_t *out_x, int16_t *out_y);
/* The vector the game's stick reader works on: WPADClampStick's circle. */
void mh3_classic_clamp(int16_t x, int16_t y, int *out_x, int *out_y);
/* The game's stick length (0x80042FDC): 0 below 30, at most 127. */
int mh3_classic_game_length(int16_t x, int16_t y);

/* PS Vita pad, as sceCtrlPeekBufferPositive2 reports it. */
enum {
    MH3_VITA_SELECT = 0x0001,
    MH3_VITA_L3 = 0x0002,
    MH3_VITA_R3 = 0x0004,
    MH3_VITA_START = 0x0008,
    MH3_VITA_UP = 0x0010,
    MH3_VITA_RIGHT = 0x0020,
    MH3_VITA_DOWN = 0x0040,
    MH3_VITA_LEFT = 0x0080,
    MH3_VITA_L2 = 0x0100,
    MH3_VITA_R2 = 0x0200,
    MH3_VITA_L1 = 0x0400, /* the handheld's L with ...Positive2 */
    MH3_VITA_R1 = 0x0800,
    MH3_VITA_TRIANGLE = 0x1000,
    MH3_VITA_CIRCLE = 0x2000,
    MH3_VITA_CROSS = 0x4000,
    MH3_VITA_SQUARE = 0x8000,
};

typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry; /* 0..255, 128 centre, y down */
    uint8_t lt, rt;         /* PS TV controllers' analog L2/R2 */
} mh3_vita_pad;

extern const mh3_stick_curve mh3_vita_stick_curve;
extern const mh3_stick_curve mh3_gamepad_stick_curve;

/* The Vita layout (docs/vita-controls.md): face buttons by position, L/R as
 * the Classic L/R triggers, Start/Select as +/-. PS TV pads add L2/R2 as ZL/ZR
 * and L3+R3 as HOME; the touch panels are not read. */
void mh3_vita_pad_to_classic(const mh3_vita_pad *pad, mh3_classic_state *out);

#ifdef __cplusplus
}
#endif

#endif
