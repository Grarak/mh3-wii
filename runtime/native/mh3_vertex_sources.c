#include "mh3_vertex_sources.h"
#include <string.h>
static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

mh3_res_status mh3_vertex_source_read(mh3_res_span bytes, uint32_t address, mh3_vertex_kind kind, mh3_native_vertex_source *out) {
    if (!out || !bytes.data || bytes.size < 32 || kind > MH3_VERTEX_UV)
        return MH3_RES_INVALID;

    const uint32_t size = word(bytes.data), offset = word(bytes.data + 8), mode = word(bytes.data + 20), format = word(bytes.data + 24);

    if (size > bytes.size || size < 32 || offset > size || offset < 32 || (uint64_t)address + size > UINT64_C(0x100000000) || (uint64_t)address + offset > UINT32_MAX)
        return MH3_RES_RANGE;

    if ((kind == MH3_VERTEX_NORMAL && mode > 2) || (kind != MH3_VERTEX_NORMAL && mode > 1))
        return MH3_RES_UNSUPPORTED;

    mh3_native_vertex_source r;
    memset(&r, 0, sizeof r);
    r.resource_address = address;
    r.payload_address = address + offset;
    r.source_id = word(bytes.data + 16);
    r.resource_bytes = size;
    r.format = format;
    r.component_mode = mode;
    r.kind = kind;
    r.count = (uint32_t)bytes.data[30] << 8 | bytes.data[31];
    r.stride = bytes.data[kind == MH3_VERTEX_COLOR ? 28 : 29];
    uint32_t value_bytes;

    if (kind == MH3_VERTEX_COLOR) {
        static const uint8_t sizes[6] = {2, 3, 4, 2, 3, 4};

        if (format >= 6)
            return MH3_RES_UNSUPPORTED;

        r.components = 4;
        value_bytes = sizes[format];
    } else {
        static const uint8_t sizes[5] = {1, 1, 2, 2, 4};

        if (format >= 5)
            return MH3_RES_UNSUPPORTED;

        r.components = kind == MH3_VERTEX_POSITION ? 2 + mode : kind == MH3_VERTEX_UV ? 1 + mode : mode == 1 ? 9 : 3;
        r.fraction = kind == MH3_VERTEX_NORMAL ? (format == 1 ? 6 : format == 3 ? 14 : 0) : bytes.data[28];

        if (r.fraction > 31)
            return MH3_RES_UNSUPPORTED;

        value_bytes = r.components * sizes[format];
    }

    if (r.stride < value_bytes)
        return MH3_RES_INVALID;

    const uint64_t required = r.count ? (uint64_t)(r.count - 1) * r.stride + value_bytes : 0;

    if (required > size - offset || required > UINT32_MAX)
        return MH3_RES_RANGE;

    r.payload_bytes = (uint32_t)required;
    *out = r;
    return MH3_RES_OK;
}

mh3_res_status mh3_vertex_fur_source_read(mh3_res_span bytes, uint32_t address, uint16_t sample, mh3_native_vertex_source *out) {
    if (!out || !bytes.data || bytes.size < 40)
        return MH3_RES_RANGE;

    const uint32_t count = word(bytes.data + 32), stride = word(bytes.data + 36);

    if (sample >= count || !word(bytes.data + 8))
        return MH3_RES_ABSENT;

    mh3_native_vertex_source r;
    mh3_res_status status = mh3_vertex_source_read(bytes, address, MH3_VERTEX_POSITION, &r);

    if (status != MH3_RES_OK)
        return status;

    const uint64_t offset = (uint64_t)word(bytes.data + 8) + (uint64_t)sample * stride;

    if (offset > r.resource_bytes || r.payload_bytes > r.resource_bytes - offset || (uint64_t)address + offset > UINT32_MAX || (count > 1 && stride < r.payload_bytes))
        return MH3_RES_RANGE;

    r.payload_address = address + (uint32_t)offset;
    r.kind = MH3_VERTEX_FUR_POSITION;
    r.sample_index = sample;
    r.sample_count = count;
    r.sample_stride = stride;
    *out = r;
    return MH3_RES_OK;
}
