#include "mh3_render_mode.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    mh3_render_mode modes[3] = {0};

    for (unsigned i = 0; i < 3; i++) {
        modes[i].framebuffer_width = 640;
        modes[i].efb_height = 480;
        modes[i].xfb_height = 480;
        modes[i].vi_width = 720 + i * 2;
        modes[i].vi_height = 480;
        modes[i].vi_x_origin = 40 + i;
    }

    for (unsigned progressive = 0; progressive < 2; progressive++) {
        for (unsigned tv = 0; tv < 7; tv++) {
            mh3_render_preferences p = {progressive, 1, tv, 0};
            mh3_render_mode current = modes[0];
            current.efb_height = 448;
            current.vi_width = 999;
            current.vi_x_origin = 19;
            const mh3_render_mode before = current;
            const mh3_res_status result = mh3_render_mode_widescreen(&p, modes, &current);
            unsigned choice = progressive ? 1 : tv == 1 ? 2 : 0;

            if (!progressive && tv != 0 && tv != 1 && tv != 5) {
                assert(result == MH3_RES_UNSUPPORTED);
                assert(memcmp(&before, &current, sizeof(current)) == 0);
            } else {
                assert(result == MH3_RES_OK);
                assert(current.vi_width == modes[choice].vi_width);
                assert(current.vi_x_origin == modes[choice].vi_x_origin);
                current.vi_width = before.vi_width;
                current.vi_x_origin = before.vi_x_origin;
                assert(memcmp(&before, &current, sizeof(current)) == 0);
            }
        }
    }

    puts("widescreen: 14 selection/preservation cases passed");
}
