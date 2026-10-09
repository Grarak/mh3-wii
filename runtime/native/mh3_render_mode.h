#ifndef MH3_RENDER_MODE_H
#define MH3_RENDER_MODE_H

#include "mh3_model_resources.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MH3_RENDER_MODE_BYTES = 60
};

enum {
    MH3_MODE_NTSC,
    MH3_MODE_PROGRESSIVE,
    MH3_MODE_PAL
};

/* Ordinary CPU display configuration. Only read/store translate the legacy
 * big endian compatibility record still consumed by translated game code. */
typedef struct {
    uint32_t tv_mode;
    uint16_t framebuffer_width, efb_height, xfb_height;
    uint16_t vi_x_origin, vi_y_origin, vi_width, vi_height;
    uint8_t reserved_geometry[2];
    uint32_t xfb_mode;
    uint8_t field_rendering, antialias;
    uint8_t sample_pattern[12][2], vertical_filter[7], reserved_tail[3];
} mh3_render_mode;

typedef struct {
    uint8_t progressive, widescreen, tv_format, component_cable;
} mh3_render_preferences;

typedef struct {
    mh3_render_mode mode;
    uint8_t pal, widescreen;
} mh3_render_configuration;

mh3_res_status mh3_render_mode_read(mh3_res_span record, mh3_render_mode *out);
mh3_res_status mh3_render_mode_store(mh3_res_mut_span record, const mh3_render_mode *mode);
/* Invalid input leaves the mode unchanged; dimensions/origins wrap at 16 bits
 * as in the SDK helper. No display, GPU, or FIFO calls occur here. */
mh3_res_status mh3_render_mode_crop(mh3_render_mode *mode, uint16_t horizontal, uint16_t vertical);
/* defaults are ordered NTSC, progressive, PAL. A custom mode preserves the
 * destination's five reserved bytes, matching 8003f9a4. */
mh3_res_status mh3_render_mode_select(const mh3_render_preferences *preferences, const mh3_render_mode defaults[3], const mh3_render_mode *custom, mh3_render_configuration *out);

/* ApplyWidemode only changes VI width/origin, retaining the current EFB and
 * other display fields. Unlike RenderInit it does not test component cable. */
mh3_res_status mh3_render_mode_widescreen(const mh3_render_preferences *preferences, const mh3_render_mode defaults[3], mh3_render_mode *current);

/* SCGetAspectRatio (804dd060): `found` is SCFindU8Item's result for IPL.AR
 * and `stored` its byte. Only a stored 1 is 16:9; a missing item or any other
 * value reads as 4:3 (0). */
uint8_t mh3_sc_aspect_ratio(int found, uint8_t stored);

#ifdef __cplusplus
}
#endif
#endif
