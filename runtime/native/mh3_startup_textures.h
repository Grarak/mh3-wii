#ifndef MH3_STARTUP_TEXTURES_H
#define MH3_STARTUP_TEXTURES_H

#include "mh3_model_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 80047954 owns three asynchronously loaded TPL groups. Groups 0/1 cycle
 * through four texture objects each; group 2 publishes consecutive objects.
 * These are CPU producer callbacks, with no graphics submission or FIFO. */
typedef struct {
    void *user;
    mh3_res_status (*archive)(void *,uint32_t group,uint32_t *count);
    mh3_res_status (*image)(void *,uint32_t group,uint32_t index,
                            mh3_native_texture_binding *binding);
    mh3_res_status (*publish)(void *,uint32_t group,uint32_t slot,
                              const mh3_native_texture_binding *binding);
} mh3_startup_texture_callbacks;

typedef struct {
    uint8_t initialized,loaded[3],selection[4];
} mh3_startup_texture_state;

/* An unready/already initialized state returns OK without invoking callbacks
 * or reading selectors. A completed run publishes the selected adjacent byte
 * pair twice, then sets initialized. Failure leaves this state unchanged;
 * callbacks may already have published a prefix, as in the original body. */
mh3_res_status mh3_startup_textures_prepare(mh3_startup_texture_state *state,
    uint32_t selector,mh3_res_span selectors,const mh3_startup_texture_callbacks *callbacks);

#ifdef __cplusplus
}
#endif
#endif
