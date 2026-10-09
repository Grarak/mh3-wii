#include "mh3_model_runtime.h"
#include <limits.h>

/* This view only adapts guest resource addresses to caller-owned native spans.
 * All parsing/validation operates on those spans, and returned addresses retain
 * the source alias. No guest execution or PPC register state is involved. */
typedef mh3_res_memory_view view;
static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint32_t canonical_address(uint32_t address) {
    return address >= UINT32_C(0xe0000000) ? address : (address & UINT32_C(0x3fffffff)) | UINT32_C(0x80000000);
}
/* Index one past the last region starting at or before the canonical address. */
static size_t region_after(const mh3_res_memory *m, uint32_t canonical) {
    size_t lo = 0, hi = m->region_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (m->regions[mid].address <= canonical)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
const mh3_res_region *mh3_res_memory_region(const mh3_res_memory *m, uint32_t address, size_t bytes, size_t *offset) {
    if (!m || !m->regions)
        return NULL;
    const uint32_t canonical = canonical_address(address);
    const size_t after = region_after(m, canonical);
    if (!after)
        return NULL;
    const mh3_res_region *r = &m->regions[after - 1];
    const size_t at = canonical - r->address;
    if (!r->span.data || at > r->span.size || bytes > r->span.size - at)
        return NULL;
    if (offset)
        *offset = at;
    return r;
}
/* RES_ABSENT: the range touches no region (map through the windows). A range
 * that runs from the windows into a region is out of range. */
static mh3_res_status mapped_region(const mh3_res_memory *m, uint32_t address, size_t bytes, view *out) {
    const uint32_t canonical = canonical_address(address);
    const size_t after = region_after(m, canonical);
    const mh3_res_region *r = after ? &m->regions[after - 1] : NULL;
    const size_t offset = r ? canonical - r->address : 0;
    if (!r || !r->span.data || offset >= r->span.size) {
        if (after < m->region_count && (uint64_t)canonical + bytes > m->regions[after].address)
            return MH3_RES_RANGE;
        return MH3_RES_ABSENT;
    }
    if (bytes > r->span.size - offset)
        return MH3_RES_RANGE;
    out->span = r->span;
    out->offset = offset;
    out->guest_base = address - (uint32_t)offset;
    return MH3_RES_OK;
}
static mh3_res_status mapped(const mh3_res_memory *m, uint32_t address, size_t bytes, view *out) {
    uint32_t physical = address & UINT32_C(0x3fffffff), offset;
    mh3_res_span span;
    if (!m || !out)
        return MH3_RES_INVALID;
    if ((address >> 30) == 1)
        return MH3_RES_RANGE;
    if (m->regions) {
        const mh3_res_status status = mapped_region(m, address, bytes, out);
        if (status != MH3_RES_ABSENT)
            return status;
    }
    if (address >= UINT32_C(0xe0000000) && address < UINT32_C(0xe0004000)) {
        offset = address - UINT32_C(0xe0000000);
        span = m->locked_cache;
    } else if (physical < UINT32_C(0x01800000)) {
        offset = physical;
        span = m->mem1;
    } else if (physical >= UINT32_C(0x10000000) && physical < UINT32_C(0x14000000)) {
        offset = physical - UINT32_C(0x10000000);
        span = m->mem2;
    } else
        return MH3_RES_RANGE;
    if (!span.data || offset > span.size || bytes > span.size - offset)
        return MH3_RES_RANGE;
    out->span = span;
    out->offset = offset;
    out->guest_base = address - offset;
    return MH3_RES_OK;
}
static mh3_res_status pointer_result(view v, size_t at, uint32_t *out) {
    if (!out || at > UINT32_MAX || at > UINT32_MAX - v.guest_base)
        return MH3_RES_RANGE;
    *out = v.guest_base + (uint32_t)at;
    return MH3_RES_OK;
}
mh3_res_status mh3_res_runtime_map(const mh3_res_memory *m, uint32_t address, size_t bytes, mh3_res_memory_view *out) {
    return mapped(m, address, bytes, out);
}
static mh3_res_status object(const mh3_res_memory *m, uint32_t ref, size_t bytes, view *v) {
    uint32_t data;
    mh3_res_status s = mh3_res_runtime_reference(m, ref, 1, &data);
    return s ? s : mapped(m, data, bytes, v);
}
static mh3_res_status model(const mh3_res_memory *m, uint32_t ref, view *v, mh3_res_model *out) {
    mh3_res_status s = object(m, ref, 16, v);
    return s ? s : mh3_res_model_open(v->span, v->offset, out);
}

mh3_res_status mh3_res_runtime_reference(const mh3_res_memory *m, uint32_t ref, int require_valid, uint32_t *out) {
    view v;
    mh3_res_status s;
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    s = mapped(m, ref, 4, &v);
    if (s)
        return s;
    *out = word(v.span.data + v.offset);
    return require_valid && !*out ? MH3_RES_INVALID : MH3_RES_OK;
}
mh3_res_status mh3_res_runtime_valid(const mh3_res_memory *m, uint32_t ref, uint32_t *out) {
    uint32_t data;
    mh3_res_status s;
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    s = mh3_res_runtime_reference(m, ref, 0, &data);
    if (s)
        return s;
    *out = data != 0;
    return MH3_RES_OK;
}
mh3_res_status mh3_res_runtime_dictionary(const mh3_res_memory *m, uint32_t ref, int32_t index, uint32_t *out) {
    uint32_t data;
    size_t at;
    view v;
    mh3_res_status s;
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    s = mh3_res_runtime_reference(m, ref, 0, &data);
    if (s || !data)
        return s;
    s = mapped(m, data, 24, &v);
    if (s)
        return s;
    s = mh3_res_dictionary_data(v.span, v.offset, index, &at);
    if (s == MH3_RES_ABSENT)
        return MH3_RES_OK;
    return s ? s : pointer_result(v, at, out);
}
mh3_res_status mh3_res_runtime_dictionary_count(const mh3_res_memory *m, uint32_t ref, uint32_t *out) {
    uint32_t data;
    view v;
    mh3_res_status s;
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    s = mh3_res_runtime_reference(m, ref, 0, &data);
    if (s || !data)
        return s;
    s = mapped(m, data, 24, &v);
    return s ? s : mh3_res_dictionary_count(v.span, v.offset, out);
}
mh3_res_status mh3_res_runtime_model(const mh3_res_memory *m, uint32_t ref, mh3_res_kind kind, int32_t index, uint32_t *out) {
    view v;
    mh3_res_model mdl;
    size_t at;
    mh3_res_status s;
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    s = model(m, ref, &v, &mdl);
    if (s)
        return s;
    s = mh3_res_model_data(&mdl, kind, index, &at);
    if (s == MH3_RES_ABSENT)
        return MH3_RES_OK;
    return s ? s : pointer_result(v, at, out);
}
mh3_res_status mh3_res_runtime_model_count(const mh3_res_memory *m, uint32_t ref, mh3_res_kind kind, uint32_t *out) {
    view v;
    mh3_res_model mdl;
    size_t at;
    mh3_res_status s;
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    s = model(m, ref, &v, &mdl);
    if (s)
        return s;
    s = mh3_res_model_dictionary(&mdl, kind, &at, out);
    return s == MH3_RES_ABSENT ? MH3_RES_OK : s;
}
mh3_res_status mh3_res_runtime_offset(const mh3_res_memory *m, uint32_t ref, int32_t delta, uint32_t alignment, uint32_t *out) {
    view v;
    uint32_t data;
    int64_t target;
    mh3_res_status s;
    if (!out || (alignment != 0 && alignment != 4))
        return MH3_RES_INVALID;
    *out = 0;
    if (!delta)
        return MH3_RES_OK;
    s = mh3_res_runtime_reference(m, ref, 0, &data);
    if (s)
        return s;
    target = (int64_t)data + delta;
    if (target < 0 || target > UINT32_MAX || (alignment && ((uint32_t)target & 3)))
        return MH3_RES_RANGE;
    s = mapped(m, (uint32_t)target, 1, &v);
    if (s)
        return s;
    *out = (uint32_t)target;
    return MH3_RES_OK;
}
mh3_res_status mh3_res_runtime_shape_parent(const mh3_res_memory *m, uint32_t ref, uint32_t *out) {
    view v;
    size_t at;
    mh3_res_status s;
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    s = object(m, ref, 8, &v);
    if (s)
        return s;
    s = mh3_res_relative(v.span, v.offset, v.offset + 4, 16, &at);
    if (s == MH3_RES_ABSENT)
        return MH3_RES_OK;
    if (s)
        return s;
    {
        mh3_res_model mdl;
        s = mh3_res_model_open(v.span, at, &mdl);
    }
    return s ? s : pointer_result(v, at, out);
}
mh3_res_status mh3_res_runtime_shape_array(const mh3_res_memory *m, uint32_t ref, uint32_t attr, uint32_t *out) {
    view v;
    size_t parent, at, field;
    int32_t index;
    mh3_res_kind kind;
    mh3_res_model mdl;
    mh3_res_status s;
    if (!out || attr >= 14)
        return MH3_RES_INVALID;
    *out = 0;
    field = 0x48 + (size_t)attr * 2;
    s = object(m, ref, field + 2, &v);
    if (s)
        return s;
    {
        uint32_t id = (uint32_t)v.span.data[v.offset + field] << 8 | v.span.data[v.offset + field + 1];
        index = id & 0x8000 ? (int32_t)id - 65536 : (int32_t)id;
    }
    if (index < 0)
        return index == -1 && attr != 0 ? MH3_RES_OK : MH3_RES_RANGE;
    s = mh3_res_relative(v.span, v.offset, v.offset + 4, 16, &parent);
    if (s)
        return s;
    s = mh3_res_model_open(v.span, parent, &mdl);
    if (s)
        return s;
    if (attr >= 12 && mdl.version != 11)
        return MH3_RES_UNSUPPORTED;
    kind = attr == 0 ? MH3_RES_POSITIONS : attr == 1 ? MH3_RES_NORMALS : attr < 4 ? MH3_RES_COLORS : attr < 12 ? MH3_RES_TEXCOORDS : attr == 12 ? MH3_RES_FUR_VECTORS : MH3_RES_FUR_POSITIONS;
    s = mh3_res_model_data(&mdl, kind, index, &at);
    if (s == MH3_RES_ABSENT)
        return MH3_RES_OK;
    return s ? s : pointer_result(v, at, out);
}
mh3_res_status mh3_res_runtime_shape_tag(const mh3_res_memory *m, uint32_t ref, int primitive, int require_valid, uint32_t *out) {
    uint32_t data, offset = primitive ? 0x24 : 0x18;
    mh3_res_status s;
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    s = mh3_res_runtime_reference(m, ref, require_valid, &data);
    if (s)
        return s;
    if (data > UINT32_MAX - offset || ((data + offset) & 3))
        return MH3_RES_RANGE;
    *out = data + offset;
    return MH3_RES_OK;
}
mh3_res_status mh3_res_runtime_shape_color(const mh3_res_memory *m, uint32_t ref, uint32_t channel, uint32_t *out) {
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    if (channel >= 2)
        return MH3_RES_RANGE;
    return mh3_res_runtime_shape_array(m, ref, 2 + channel, out);
}
mh3_res_status mh3_res_runtime_shape_texcoord(const mh3_res_memory *m, uint32_t ref, uint32_t channel, uint32_t *out) {
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    if (channel >= 8)
        return MH3_RES_RANGE;
    return mh3_res_runtime_shape_array(m, ref, 4 + channel, out);
}
mh3_res_status mh3_res_runtime_tag_list(const mh3_res_memory *m, uint32_t ref, uint32_t *out) {
    view v;
    size_t at;
    mh3_res_status s;
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    s = object(m, ref, 12, &v);
    if (s)
        return s;
    s = mh3_res_relative(v.span, v.offset, v.offset + 8, 1, &at);
    if (s == MH3_RES_ABSENT)
        return MH3_RES_OK;
    return s ? s : pointer_result(v, at, out);
}
mh3_res_status mh3_res_runtime_shape_setup(const mh3_res_memory *m, uint32_t ref, uint32_t *out) {
    view v;
    size_t at;
    mh3_res_status s;
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    s = object(m, ref, 0x24, &v);
    if (s)
        return s;
    s = mh3_res_relative(v.span, v.offset + 0x18, v.offset + 0x20, 1, &at);
    if (s == MH3_RES_ABSENT)
        return MH3_RES_OK;
    return s ? s : pointer_result(v, at, out);
}
mh3_res_status mh3_res_runtime_word(const mh3_res_memory *m, uint32_t ref, uint32_t field, uint32_t *out) {
    view v;
    mh3_res_status s;
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    if (field > UINT32_MAX - 4)
        return MH3_RES_RANGE;
    s = object(m, ref, (size_t)field + 4, &v);
    if (s)
        return s;
    *out = word(v.span.data + v.offset + field);
    return MH3_RES_OK;
}
mh3_res_status mh3_res_runtime_shape_visible(const mh3_res_memory *m, uint32_t ref, uint32_t *out) {
    uint32_t flags;
    mh3_res_status s;
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    s = mh3_res_runtime_word(m, ref, 0x34, &flags);
    if (s)
        return s;
    *out = !(flags & 2);
    return MH3_RES_OK;
}
mh3_res_status mh3_res_runtime_node_visible(const mh3_res_memory *m, uint32_t ref, uint32_t *out) {
    uint32_t data;
    view v;
    mh3_res_status s;
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    s = mh3_res_runtime_reference(m, ref, 1, &data);
    if (s)
        return s;
    s = mapped(m, data, 0x18, &v);
    if (s)
        return s;
    *out = (word(v.span.data + v.offset + 0x14) & 0x100) != 0;
    return MH3_RES_OK;
}
mh3_res_status mh3_res_runtime_shape_has_attribute(const mh3_res_memory *m, uint32_t ref, uint32_t attr, uint32_t *out) {
    uint32_t bits;
    mh3_res_status s;
    if (!out)
        return MH3_RES_INVALID;
    *out = 0;
    s = mh3_res_runtime_word(m, ref, 0x30, &bits);
    if (s)
        return s;
    if (attr >= 32)
        return MH3_RES_RANGE;
    *out = (bits & (UINT32_C(1) << attr)) != 0;
    return MH3_RES_OK;
}
