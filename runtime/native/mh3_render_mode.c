/* Native C reconstruction of RMHP08 RenderInit (8003f794), its mode copy
 * (8003f9a4), overscan arithmetic (804b7280) and SCGetAspectRatio (804dd060).
 * The SDK GX entry remains a fatal tripwire; this core neither submits
 * graphics nor emulates registers. */
#include "mh3_render_mode.h"
#include <string.h>

uint8_t mh3_sc_aspect_ratio(int found, uint8_t stored) {
    return found && stored == 1 ? 1 : 0;
}

mh3_res_status mh3_render_mode_widescreen(const mh3_render_preferences *preferences, const mh3_render_mode defaults[3], mh3_render_mode *current) {
    if (!preferences || !defaults || !current)
        return MH3_RES_INVALID;

    mh3_render_preferences selection = *preferences;
    selection.component_cable = 1;
    mh3_render_configuration selected = {0};
    const mh3_res_status status = mh3_render_mode_select(&selection, defaults, NULL, &selected);

    if (status != MH3_RES_OK)
        return status;

    current->vi_width = selected.mode.vi_width;
    current->vi_x_origin = selected.mode.vi_x_origin;
    return MH3_RES_OK;
}

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void write16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void write32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

mh3_res_status mh3_render_mode_read(mh3_res_span record, mh3_render_mode *out) {
    if (!out || !record.data || record.size < MH3_RENDER_MODE_BYTES)
        return MH3_RES_RANGE;

    const uint8_t *p = record.data;
    mh3_render_mode mode;
    mode.tv_mode = read32(p);
    mode.framebuffer_width = read16(p + 4);
    mode.efb_height = read16(p + 6);
    mode.xfb_height = read16(p + 8);
    mode.vi_x_origin = read16(p + 10);
    mode.vi_y_origin = read16(p + 12);
    mode.vi_width = read16(p + 14);
    mode.vi_height = read16(p + 16);
    memcpy(mode.reserved_geometry, p + 18, 2);
    mode.xfb_mode = read32(p + 20);
    mode.field_rendering = p[24];
    mode.antialias = p[25];
    memcpy(mode.sample_pattern, p + 26, 24);
    memcpy(mode.vertical_filter, p + 50, 7);
    memcpy(mode.reserved_tail, p + 57, 3);
    *out = mode;
    return MH3_RES_OK;
}

mh3_res_status mh3_render_mode_store(mh3_res_mut_span record, const mh3_render_mode *mode) {
    if (!mode || !record.data || record.size < MH3_RENDER_MODE_BYTES)
        return MH3_RES_RANGE;
    /* Snapshot also permits the caller's typed object to alias this span. */
    const mh3_render_mode value = *mode;
    uint8_t *p = record.data;
    write32(p, value.tv_mode);
    write16(p + 4, value.framebuffer_width);
    write16(p + 6, value.efb_height);
    write16(p + 8, value.xfb_height);
    write16(p + 10, value.vi_x_origin);
    write16(p + 12, value.vi_y_origin);
    write16(p + 14, value.vi_width);
    write16(p + 16, value.vi_height);
    memcpy(p + 18, value.reserved_geometry, 2);
    write32(p + 20, value.xfb_mode);
    p[24] = value.field_rendering;
    p[25] = value.antialias;
    memcpy(p + 26, value.sample_pattern, 24);
    memcpy(p + 50, value.vertical_filter, 7);
    memcpy(p + 57, value.reserved_tail, 3);
    return MH3_RES_OK;
}

mh3_res_status mh3_render_mode_crop(mh3_render_mode *mode, uint16_t horizontal, uint16_t vertical) {
    if (!mode || !mode->xfb_height)
        return MH3_RES_INVALID;

    const mh3_render_mode original = *mode;
    const uint32_t h = (uint32_t)horizontal * 2;
    const uint32_t v = (uint32_t)(vertical & 0x7fff) * 2;
    const uint32_t scan = original.tv_mode & 3;
    mode->framebuffer_width = (uint16_t)(original.framebuffer_width - h);
    mode->efb_height = (uint16_t)(original.efb_height - v * original.efb_height / original.xfb_height);
    mode->xfb_height = (uint16_t)(original.xfb_height - (original.xfb_mode == 0 && scan == 0 ? v / 2 : v));
    mode->vi_width = (uint16_t)(original.vi_width - h);
    mode->vi_height = (uint16_t)(original.vi_height - (scan == 1 ? v * 2 : v));
    mode->vi_x_origin = (uint16_t)(original.vi_x_origin + horizontal);
    /* The reduction masks bit 15; the origin addition uses the full value. */
    mode->vi_y_origin = (uint16_t)(original.vi_y_origin + vertical);
    return MH3_RES_OK;
}

mh3_res_status mh3_render_mode_select(const mh3_render_preferences *preferences, const mh3_render_mode defaults[3], const mh3_render_mode *custom, mh3_render_configuration *out) {
    if (!preferences || !out || (!custom && !defaults))
        return MH3_RES_INVALID;

    mh3_render_configuration result = {0};
    result.pal = 0;
    result.widescreen = preferences->widescreen == 1;

    if (custom) {
        uint8_t geometry[2], tail[3];
        memcpy(geometry, out->mode.reserved_geometry, 2);
        memcpy(tail, out->mode.reserved_tail, 3);
        result.mode = *custom;
        memcpy(result.mode.reserved_geometry, geometry, 2);
        memcpy(result.mode.reserved_tail, tail, 3);
    } else {
        uint16_t overscan = 16;

        if (preferences->progressive == 1 && preferences->component_cable == 1) {
            result.mode = defaults[MH3_MODE_PROGRESSIVE];
        } else if (preferences->tv_format == 0 || preferences->tv_format == 5) {
            result.mode = defaults[MH3_MODE_NTSC];
        } else if (preferences->tv_format == 1) {
            result.mode = defaults[MH3_MODE_PAL];
            result.pal = 1;
            overscan = 40;
        } else {
            return MH3_RES_UNSUPPORTED;
        }

        mh3_res_status status = mh3_render_mode_crop(&result.mode, 0, overscan);

        if (status != MH3_RES_OK)
            return status;

        if (preferences->progressive == 1 && preferences->component_cable == 1) {
            if (preferences->tv_format != 0)
                result.mode.tv_mode = 0x16;
        } else if (preferences->tv_format == 5) {
            result.mode.tv_mode = 0x14;
        } else if (result.pal) {
            /* PAL only crops the EFB. Its VI/XFB height and VI origin stay at
             * the source values in RenderInit's post-overscan fixup. */
            result.mode.xfb_height = defaults[MH3_MODE_PAL].xfb_height;
            result.mode.vi_y_origin = defaults[MH3_MODE_PAL].vi_y_origin;
            result.mode.vi_height = defaults[MH3_MODE_PAL].vi_height;
        }
    }

    *out = result;
    return MH3_RES_OK;
}
