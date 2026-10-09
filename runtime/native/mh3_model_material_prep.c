/* RMHP08 resource material preparation. Ordinary bounded C11 data effects;
 * neither guest register emulation nor GX/FIFO submission belongs here. */
#include "mh3_model_material_prep.h"
#include <string.h>

static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

mh3_res_status mh3_model_genmode_read(mh3_res_span bytes, mh3_model_genmode *out) {
    if (!bytes.data || !out)
        return MH3_RES_INVALID;

    if (bytes.size < 8)
        return MH3_RES_RANGE;

    const mh3_model_genmode result = {bytes.data[0], bytes.data[1], bytes.data[2], bytes.data[3], word(bytes.data + 4), 0};

    if (result.texgens > 8 || result.channels > 2 || !result.tevs || result.tevs > 16 || result.indirect > 4 || result.cull > 3)
        return MH3_RES_UNSUPPORTED;

    *out = result;
    return MH3_RES_OK;
}

mh3_res_status mh3_model_genmode_select(mh3_res_mut_span cache, mh3_res_span resource) {
    if (!cache.data)
        return MH3_RES_INVALID;

    if (cache.size < 12)
        return MH3_RES_RANGE;

    mh3_model_genmode value;
    mh3_res_status status = mh3_model_genmode_read(resource, &value);

    if (status)
        return status;

    uint32_t flags = word(cache.data + 8);

    if (cache.data[0] != value.texgens || cache.data[1] != value.channels)
        flags &= ~3u;

    if (cache.data[2] != value.tevs || cache.data[3] != value.indirect || word(cache.data + 4) != value.cull)
        flags &= ~1u;

    if (!(flags & MH3_GENMODE_SELECTED))
        flags = (flags & ~3u) | MH3_GENMODE_SELECTED;

    cache.data[0] = value.texgens;
    cache.data[1] = value.channels;
    cache.data[2] = value.tevs;
    cache.data[3] = value.indirect;
    put(cache.data + 4, value.cull);
    put(cache.data + 8, flags);
    return MH3_RES_OK;
}

mh3_res_status mh3_model_genmode_publish(mh3_res_mut_span cache, int xf, mh3_native_material_state *out, uint32_t *known, int *apply) {
    if (!out)
        return MH3_RES_INVALID;

    mh3_model_genmode value;
    const mh3_res_status status = mh3_model_genmode_publish_counts(cache, xf, &value, known, apply);

    if (status || !*apply)
        return status;

    memset(out, 0, sizeof *out);
    out->texgen_count = value.texgens;
    out->channel_count = value.channels;
    out->tev_count = value.tevs;
    out->indirect_count = value.indirect;
    out->cull_mode = value.cull;
    return MH3_RES_OK;
}

mh3_res_status mh3_model_genmode_publish_counts(mh3_res_mut_span cache, int xf, mh3_model_genmode *out, uint32_t *known, int *apply) {
    if (!cache.data || !out || !known || !apply || (xf != 0 && xf != 1))
        return MH3_RES_INVALID;

    if (cache.size < 12)
        return MH3_RES_RANGE;

    const uint32_t flags = word(cache.data + 8), loaded = flags & 3u;

    if (!(flags & MH3_GENMODE_SELECTED) || (xf ? loaded == 3 : loaded != 2)) {
        *apply = 0;
        return MH3_RES_OK;
    }

    mh3_model_genmode value;
    mh3_res_status status = mh3_model_genmode_read((mh3_res_span){cache.data, cache.size}, &value);

    if (status)
        return status;

    const uint32_t mask = MH3_MATERIAL_TOP_TEV_COUNT | MH3_MATERIAL_TOP_INDIRECT_COUNT | MH3_MATERIAL_TOP_CULL | (xf ? (MH3_MATERIAL_TOP_TEXGEN_COUNT | MH3_MATERIAL_TOP_CHANNEL_COUNT) : 0);
    put(cache.data + 8, flags | (xf ? 3u : 1u));
    *out = value;
    *known = mask;
    *apply = 1;
    return MH3_RES_OK;
}

mh3_res_status mh3_model_material_misc_read(mh3_res_span bytes, mh3_model_material_misc *out) {
    if (!bytes.data || !out)
        return MH3_RES_INVALID;

    if (bytes.size < 12)
        return MH3_RES_RANGE;

    mh3_model_material_misc result;
    memset(&result, 0, sizeof result);
    result.depth_before_texture = bytes.data[0] != 0;
    result.light_reference = (int8_t)(bytes.data[1] < 128 ? bytes.data[1] : (int)bytes.data[1] - 256);
    result.fog_reference = (int8_t)(bytes.data[2] < 128 ? bytes.data[2] : (int)bytes.data[2] - 256);

    for (unsigned i = 0; i < 3; i++) {
        result.indirect_method[i] = bytes.data[4 + i];
        result.normal_map_light[i] = (int8_t)(bytes.data[8 + i] < 128 ? bytes.data[8 + i] : (int)bytes.data[8 + i] - 256);
    }

    *out = result;
    return MH3_RES_OK;
}

static void channel_color(mh3_pack_channel channels[2], uint32_t selector, const uint8_t rgba[4], int ambient) {
    mh3_pack_channel *channel = &channels[selector & 1u];

    if (ambient) {
        memcpy(channel->ambient_color, rgba, 4);
        channel->flags |= MH3_CHANNEL_AMBIENT_COLOR | MH3_CHANNEL_AMBIENT_ALPHA;
    } else {
        memcpy(channel->material_color, rgba, 4);
        channel->flags |= MH3_CHANNEL_MATERIAL_COLOR | MH3_CHANNEL_MATERIAL_ALPHA;
    }
}

mh3_res_status mh3_model_channel_color(mh3_native_material_state *patch, uint32_t selector, const uint8_t rgba[4], int ambient) {
    if (!patch || !rgba || (ambient != 0 && ambient != 1))
        return MH3_RES_INVALID;

    channel_color(patch->channel, selector, rgba, ambient);
    return MH3_RES_OK;
}

static mh3_res_status channel_control(mh3_pack_channel channels[2], uint32_t selector, uint32_t raw, uint32_t mask, int force_off) {
    raw = (raw & (force_off ? 0xffff87c1u : 0xffff87c3u)) | ((mask & 15u) << 2) | ((mask >> 4 & 15u) << 11);
    const int select = (raw & (1u << 10)) != 0;
    const uint8_t diffuse = select ? (uint8_t)(raw >> 7 & 3u) : 0;

    if (diffuse > 2)
        return MH3_RES_UNSUPPORTED;

    const mh3_pack_channel_control value = {(uint8_t)(raw & 1u), (uint8_t)(raw >> 6 & 1u), (uint8_t)(raw >> 1 & 1u), diffuse, (uint8_t)(!select ? 0 : raw & (1u << 9) ? 1 : 2), (uint8_t)(mask & 255u)};
    mh3_pack_channel *channel = &channels[selector & 1u];

    if (selector & 2u) {
        channel->alpha = value;
        channel->raw_alpha_control = raw;
        channel->flags |= MH3_CHANNEL_CONTROL_ALPHA;
    } else {
        channel->color = value;
        channel->raw_color_control = raw;
        channel->flags |= MH3_CHANNEL_CONTROL_COLOR;
    }

    return MH3_RES_OK;
}

mh3_res_status mh3_model_channel_control(mh3_native_material_state *patch, uint32_t selector, uint32_t raw, uint32_t mask, int force_off) {
    if (!patch || (force_off != 0 && force_off != 1))
        return MH3_RES_INVALID;

    return channel_control(patch->channel, selector, raw, mask, force_off);
}

mh3_res_status mh3_model_channels_prepare(mh3_res_span bytes, uint32_t count, const uint32_t masks[4], const uint8_t ambient[4], int force_off, mh3_native_material_state *out) {
    if (!out)
        return MH3_RES_INVALID;

    mh3_pack_channel channels[2];
    const mh3_res_status status = mh3_model_channels_prepare_channels(bytes, count, masks, ambient, force_off, channels);

    if (status)
        return status;

    memset(out, 0, sizeof *out);
    memcpy(out->channel, channels, sizeof channels);
    return MH3_RES_OK;
}

mh3_res_status mh3_model_channels_prepare_channels(mh3_res_span bytes, uint32_t count, const uint32_t masks[4], const uint8_t ambient[4], int force_off, mh3_pack_channel out[2]) {
    if (!bytes.data || !masks || !ambient || !out || count > 2 || (force_off != 0 && force_off != 1))
        return MH3_RES_INVALID;

    if (bytes.size < 40)
        return MH3_RES_RANGE;

    mh3_pack_channel patch[2];
    memset(patch, 0, sizeof patch);

    for (unsigned channel = 0; channel < (count == 2 ? 2u : 1u); channel++) {
        const uint8_t *p = bytes.data + channel * 20;
        const uint32_t flags = word(p);

        if (flags & 3u)
            channel_color(patch, channel, p + 4, 0);

        if (flags & 12u) {
            uint8_t color[4];

            for (unsigned i = 0; i < 4; i++) {
                /* 80086C78/80 use separate fmuls/fadds. The build uses
                 * -ffp-contract=off for this exact single-precision order. */
                const float scaled = (float)((uint32_t)ambient[i] * p[8 + i]) * 0x1.010102p-8f;
                color[i] = channel ? p[8 + i] : (uint8_t)(0.5f + scaled);
            }

            channel_color(patch, channel, color, 1);
        }

        for (unsigned pass = 0; pass < 2; pass++)
            if (flags & (16u << pass)) {
                const mh3_res_status status = channel_control(patch, channel + pass * 2, word(p + 12 + pass * 4), masks[channel * 2 + pass], force_off);

                if (status)
                    return status;
            }
    }

    if (count != 2) {
        channel_control(patch, 1, 0, 0, 0);
        channel_control(patch, 3, 0, 0, 0);
    }

    memcpy(out, patch, sizeof patch);
    return MH3_RES_OK;
}
