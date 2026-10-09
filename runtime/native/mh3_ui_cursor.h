#ifndef MH3_UI_CURSOR_H
#define MH3_UI_CURSOR_H
#include "mh3_draw_state.h"
#include "mh3_model_resources.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {
    MH3_CURSOR_ROOT,
    MH3_CURSOR_ENABLED,
    MH3_CURSOR_BLOCKED,
    MH3_CURSOR_MODE,
    MH3_CURSOR_SLOT,
    MH3_CURSOR_HORIZONTAL,
    MH3_CURSOR_ROW,
    MH3_CURSOR_COLUMN,
    MH3_CURSOR_VERTICAL,
    MH3_CURSOR_WIDESCREEN
};
typedef struct {
    void *user;
    void (*raster_read)(void *, mh3_raster_state *);
    void (*raster_publish)(void *, const mh3_raster_state *);
    void (*clear_depth)(void *);
    uint32_t (*field)(void *, unsigned);
    uint32_t (*virtual_call)(void *, uint32_t object, uint32_t slot);
    mh3_res_status (*position)(void *, uint32_t mode, uint32_t row, uint32_t column, mh3_res_span *);
    void (*draw_cursor)(void *, int32_t x, int32_t y);
} mh3_ui_cursor_callbacks;

/* 80449800: publish the depth-clear raster state, clear, draw the UI root,
 * reset depth/blend state, query eligibility and optionally draw its cursor.
 * Fields are read after the appropriate virtual call, rather than snapshotted
 * before UI drawing. All CPU callbacks still run on a skipped GPU frame. */
mh3_res_status mh3_ui_cursor_draw(const mh3_ui_cursor_callbacks *callbacks);

/* Signed-halfword publications follow PPC wrap/truncation, including fctiwz's
 * saturation when a rounded single-precision value exceeds int32. */
void mh3_ui_cursor_coordinates(int32_t authored_x, int32_t authored_y, uint32_t horizontal_bits, uint32_t vertical_bits, int widescreen, int32_t out[2]);

#ifdef __cplusplus
}
#endif
#endif
