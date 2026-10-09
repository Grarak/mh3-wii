/* RMHP08 80449800, reconstructed from cached Ghidra C and the complete
 * instruction listing. GPU ownership and guest memory remain at the ABI. */
#include "mh3_ui_cursor.h"

static int32_t signed16(uint32_t value) {
    return (int32_t)(value & 65535u) - ((value & 32768u) ? 65536 : 0);
}
static int32_t signed32(uint32_t value) {
    return value <= INT32_MAX ? (int32_t)value : -(int32_t)(~value) - 1;
}
void mh3_ui_cursor_coordinates(int32_t authored_x, int32_t authored_y, uint32_t horizontal_bits, uint32_t vertical_bits, int widescreen, int32_t out[2]) {
    const float horizontal = (float)signed32(horizontal_bits);
    const float adjusted = widescreen ? horizontal * 0.5f : horizontal;
    const int32_t truncated = adjusted >= 2147483648.f ? INT32_MAX : adjusted <= -2147483648.f ? INT32_MIN : (int32_t)adjusted;
    out[0] = signed16((uint32_t)authored_x + (uint32_t)truncated + (widescreen ? 418u : 304u));
    out[1] = signed16((uint32_t)authored_y - (uint32_t)signed16(vertical_bits) + 228u);
}
mh3_res_status mh3_ui_cursor_draw(const mh3_ui_cursor_callbacks *io) {
    if (!io || !io->raster_read || !io->raster_publish || !io->clear_depth || !io->field || !io->virtual_call || !io->position || !io->draw_cursor)
        return MH3_RES_INVALID;
    mh3_raster_state raster;
    io->raster_read(io->user, &raster);
    raster.color_write = raster.alpha_write = 0;
    raster.depth_test = 1;
    raster.depth_compare = 7;
    raster.depth_write = 1;
    io->raster_publish(io->user, &raster);
    io->clear_depth(io->user);
    io->raster_read(io->user, &raster);
    raster.color_write = raster.alpha_write = 1;
    raster.depth_test = 0;
    raster.depth_compare = 3;
    raster.depth_write = 0;
    io->raster_publish(io->user, &raster);
    io->virtual_call(io->user, io->field(io->user, MH3_CURSOR_ROOT), 0x18);
    io->raster_read(io->user, &raster);
    raster.depth_test = 0;
    raster.depth_compare = 3;
    raster.depth_write = 0;
    raster.blend_mode = 1;
    raster.src_factor = 4;
    raster.dst_factor = 5;
    raster.logic_op = 0;
    io->raster_publish(io->user, &raster);
    if (!io->field(io->user, MH3_CURSOR_ENABLED) || io->field(io->user, MH3_CURSOR_BLOCKED))
        return MH3_RES_OK;
    const uint32_t child = io->virtual_call(io->user, io->field(io->user, MH3_CURSOR_ROOT), 0x78);
    if (io->virtual_call(io->user, child, 0x48))
        return MH3_RES_OK;
    const uint32_t mode = io->field(io->user, MH3_CURSOR_MODE);
    if (mode != 1 && mode != 2)
        return MH3_RES_OK;
    const uint32_t column = io->field(io->user, mode == 1 ? MH3_CURSOR_SLOT : MH3_CURSOR_COLUMN);
    const uint32_t row = io->field(io->user, mode == 1 ? MH3_CURSOR_HORIZONTAL : MH3_CURSOR_ROW);
    mh3_res_span position = {0};
    const mh3_res_status status = io->position(io->user, mode, row, column, &position);
    if (status != MH3_RES_OK)
        return status;
    if (!position.data || position.size < 4)
        return MH3_RES_RANGE;
    const int32_t x = signed16((uint32_t)position.data[0] << 8 | position.data[1]);
    const uint32_t vertical = io->field(io->user, MH3_CURSOR_VERTICAL);
    const int32_t y = signed16((uint32_t)position.data[2] << 8 | position.data[3]);
    const int widescreen = io->field(io->user, MH3_CURSOR_WIDESCREEN) != 0;
    int32_t coordinates[2];
    mh3_ui_cursor_coordinates(x, y, io->field(io->user, MH3_CURSOR_HORIZONTAL), vertical, widescreen, coordinates);
    io->draw_cursor(io->user, coordinates[0], coordinates[1]);
    return MH3_RES_OK;
}
