/* RMHP08 80047954, checked against Ghidra C and the original instruction
 * listing. SDK object construction becomes typed CPU texture publication. */
#include "mh3_startup_textures.h"

mh3_res_status mh3_startup_textures_prepare(mh3_startup_texture_state *state, uint32_t selector, mh3_res_span selectors, const mh3_startup_texture_callbacks *callbacks) {
    if (!state)
        return MH3_RES_INVALID;
    if (state->initialized || state->loaded[0] != 2 || state->loaded[1] != 2 || state->loaded[2] != 2)
        return MH3_RES_OK;
    if (!callbacks || !callbacks->archive || !callbacks->image || !callbacks->publish)
        return MH3_RES_INVALID;
    if (!selectors.data || selector >= selectors.size)
        return MH3_RES_RANGE;
    for (uint32_t group = 0; group < 3; ++group) {
        uint32_t count = 0;
        mh3_res_status status = callbacks->archive(callbacks->user, group, &count);
        if (status != MH3_RES_OK)
            return status;
        /* TPLBind's original 16-bit counter cannot finish larger archives. */
        if (count > UINT16_MAX)
            return MH3_RES_RANGE;
        for (uint32_t index = 0; index < count; ++index) {
            mh3_native_texture_binding binding = {0};
            status = callbacks->image(callbacks->user, group, index, &binding);
            if (status != MH3_RES_OK)
                return status;
            binding.source.min_lod = 0;
            binding.source.max_lod = 0;
            binding.source.levels = 1;
            if (group == 2)
                binding.sampler.wrap_s = binding.sampler.wrap_t = 0;
            binding.sampler.min_filter = 0;
            binding.sampler.mag_filter = 1;
            binding.sampler.lod_bias = 0;
            binding.sampler.bias_clamp = 0;
            binding.sampler.edge_lod = 0;
            binding.sampler.anisotropy = 0;
            status = callbacks->publish(callbacks->user, group, group < 2 ? index % 4 : index, &binding);
            if (status != MH3_RES_OK)
                return status;
        }
    }
    const uint8_t first = selectors.data[selector], second = (uint8_t)(first + 1u);
    state->selection[0] = state->selection[2] = first;
    state->selection[1] = state->selection[3] = second;
    state->initialized = 1;
    return MH3_RES_OK;
}
