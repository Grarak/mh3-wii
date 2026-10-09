#include "mh3_model_resources.h"
#include <limits.h>
#include <string.h>

static int fits(mh3_res_span s, size_t at, size_t n) {
    return s.data != NULL && at <= s.size && n <= s.size - at;
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int64_t signed32(uint32_t v) {
    return (v & UINT32_C(0x80000000)) ? (int64_t)v - INT64_C(0x100000000) : (int64_t)v;
}

static int16_t signed16(uint16_t v) {
    return (int16_t)((v & 0x8000u) ? (int32_t)v - 0x10000 : v);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int model_fits(const mh3_res_model *m, size_t at, size_t n) {
    return m && at >= m->offset && at - m->offset <= m->size && n <= m->size - (at - m->offset) && fits(m->container, at, n);
}

/* ResOffsetToPtr 80097dfc, with checked native offsets instead of PPC pointers. */
mh3_res_status mh3_res_relative(mh3_res_span s, size_t base, size_t field, size_t required, size_t *result) {
    int64_t delta;
    size_t target;

    if (!result)
        return MH3_RES_INVALID;

    *result = 0;

    if (!fits(s, field, 4) || base > s.size)
        return MH3_RES_RANGE;

    delta = signed32(be32(s.data + field));

    if (!delta)
        return MH3_RES_ABSENT;

    if (delta < 0) {
        if ((uint64_t)(-delta) > base)
            return MH3_RES_RANGE;

        target = base - (size_t)(-delta);
    } else {
        if ((uint64_t)delta > s.size - base)
            return MH3_RES_RANGE;

        target = base + (size_t)delta;
    }

    if (!fits(s, target, required))
        return MH3_RES_RANGE;

    *result = target;
    return MH3_RES_OK;
}

mh3_res_status mh3_res_model_open(mh3_res_span s, size_t at, mh3_res_model *out) {
    uint32_t size, revision;

    if (!out)
        return MH3_RES_INVALID;

    memset(out, 0, sizeof(*out));

    if (!fits(s, at, 16))
        return MH3_RES_RANGE;

    if (memcmp(s.data + at, "MDL0", 4))
        return MH3_RES_INVALID;

    size = be32(s.data + at + 4);
    revision = be32(s.data + at + 8);

    if (revision != 9 && revision != 11)
        return MH3_RES_UNSUPPORTED;

    if (size < (revision == 11 ? 0x8cu : 0x80u))
        return MH3_RES_INVALID;

    if (!fits(s, at, size))
        return MH3_RES_RANGE;

    out->container = s;
    out->offset = at;
    out->size = size;
    out->version = revision;
    return MH3_RES_OK;
}

mh3_res_status mh3_res_dictionary_count(mh3_res_span s, size_t at, uint32_t *count) {
    uint32_t size, n;

    if (!count)
        return MH3_RES_INVALID;

    *count = 0;

    if (!fits(s, at, 24))
        return MH3_RES_RANGE;

    size = be32(s.data + at);
    n = be32(s.data + at + 4);

    if (size < 24 || n > (size - 24) / 16)
        return MH3_RES_INVALID;

    if (!fits(s, at, size))
        return MH3_RES_RANGE;

    *count = n;
    return MH3_RES_OK;
}

mh3_res_status mh3_res_dictionary_data(mh3_res_span s, size_t at, int32_t index, size_t *out) {
    uint32_t count;
    mh3_res_status status;

    if (!out)
        return MH3_RES_INVALID;

    *out = 0;
    status = mh3_res_dictionary_count(s, at, &count);

    if (status)
        return status;

    if (index < 0 || (uint32_t)index >= count)
        return MH3_RES_RANGE;

    return mh3_res_relative(s, at, at + 24 + (size_t)index * 16 + 12, 1, out);
}

static mh3_res_status dictionary_field(const mh3_res_model *m, mh3_res_kind k, size_t *field) {
    static const uint8_t offsets[] = {0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38};
    uint8_t off;

    if (!m || !field || (m->version != 9 && m->version != 11))
        return MH3_RES_INVALID;

    if (k == MH3_RES_FUR_VECTORS || k == MH3_RES_FUR_POSITIONS) {
        if (m->version != 11)
            return MH3_RES_ABSENT;

        off = k == MH3_RES_FUR_VECTORS ? 0x28 : 0x2c;
    } else {
        if ((unsigned)k >= sizeof(offsets))
            return MH3_RES_INVALID;

        off = offsets[k];

        if (m->version == 11 && k >= MH3_RES_MATERIALS)
            off += 8;
    }

    *field = m->offset + off;
    return MH3_RES_OK;
}

mh3_res_status mh3_res_model_dictionary(const mh3_res_model *m, mh3_res_kind kind, size_t *at, uint32_t *count) {
    size_t field, dictionary_size;
    mh3_res_status status;
    uint32_t n;

    if (!at || !count)
        return MH3_RES_INVALID;

    *at = 0;
    *count = 0;
    status = dictionary_field(m, kind, &field);

    if (status != MH3_RES_OK)
        return status;

    status = mh3_res_relative(m->container, m->offset, field, 24, at);

    if (status != MH3_RES_OK)
        return status;

    if (!model_fits(m, *at, 24))
        return MH3_RES_RANGE;

    dictionary_size = be32(m->container.data + *at);
    n = be32(m->container.data + *at + 4);

    if (dictionary_size < 24 || n > (dictionary_size - 24) / 16)
        return MH3_RES_INVALID;

    if (!model_fits(m, *at, dictionary_size))
        return MH3_RES_RANGE;

    *count = n;
    return MH3_RES_OK;
}

mh3_res_status mh3_res_model_data(const mh3_res_model *m, mh3_res_kind kind, int32_t index, size_t *out) {
    size_t dic;
    uint32_t count;
    mh3_res_status status;

    if (!out)
        return MH3_RES_INVALID;

    *out = 0;
    status = mh3_res_model_dictionary(m, kind, &dic, &count);

    if (status)
        return status;

    status = mh3_res_dictionary_data(m->container, dic, index, out);

    if (status)
        return status;

    return model_fits(m, *out, 1) ? MH3_RES_OK : MH3_RES_RANGE;
}

/* ResDicLookup 800627b4 plus node 80097f14, position 8009802c,
 * normal 80098144, color 8009825c, texcoord 800983e0,
 * material 800987fc, shape 80098914 and fur-position 80098680. */
mh3_res_status mh3_res_model_entry(const mh3_res_model *m, mh3_res_kind kind, uint32_t id, mh3_res_entry *out) {
    size_t dic, record, target, string_at;
    uint32_t count;
    mh3_res_status status;

    if (!out)
        return MH3_RES_INVALID;

    memset(out, 0, sizeof(*out));
    status = mh3_res_model_dictionary(m, kind, &dic, &count);

    if (status != MH3_RES_OK)
        return status;

    if (id >= count)
        return MH3_RES_RANGE;

    record = dic + 24 + (size_t)id * 16;
    status = mh3_res_relative(m->container, dic, record + 12, 1, &target);

    if (status != MH3_RES_OK)
        return status == MH3_RES_ABSENT ? MH3_RES_INVALID : status;

    if (!model_fits(m, target, 1))
        return MH3_RES_RANGE;

    status = mh3_res_relative(m->container, dic, record + 8, 1, &string_at);

    if (status != MH3_RES_OK)
        return status == MH3_RES_ABSENT ? MH3_RES_INVALID : status;

    if (!memchr(m->container.data + string_at, 0, m->container.size - string_at))
        return MH3_RES_INVALID;

    out->id = id;
    out->offset = target;
    out->name = (const char *)(m->container.data + string_at);
    return MH3_RES_OK;
}

mh3_res_status mh3_res_model_find(const mh3_res_model *m, mh3_res_kind kind, const char *name, mh3_res_entry *out) {
    size_t at;
    uint32_t count, i;
    mh3_res_status status;

    if (!name || !out)
        return MH3_RES_INVALID;

    memset(out, 0, sizeof(*out));
    status = mh3_res_model_dictionary(m, kind, &at, &count);

    if (status != MH3_RES_OK)
        return status;

    for (i = 0; i < count; ++i) {
        status = mh3_res_model_entry(m, kind, i, out);

        if (status != MH3_RES_OK)
            return status;

        if (!strcmp(out->name, name))
            return MH3_RES_OK;
    }

    memset(out, 0, sizeof(*out));
    return MH3_RES_ABSENT;
}

/* Tag references 80099604/8009a15c; DL 8009974c; cmdSize 8009a07c;
 * bufSize 80099e38. Checked reference wrappers 80099578/800997a4 and
 * typed constructors 80099638/8009a0a0 collapse to this native value type. */
static mh3_res_status display_list(const mh3_res_model *m, size_t tag, size_t shape_at, size_t shape_size, mh3_res_display_list *out) {
    mh3_res_status status;
    size_t dl;

    if (!model_fits(m, tag, 12))
        return MH3_RES_RANGE;

    out->buffer_size = be32(m->container.data + tag);
    out->command_size = be32(m->container.data + tag + 4);

    if (out->command_size > out->buffer_size)
        return MH3_RES_INVALID;

    status = mh3_res_relative(m->container, tag, tag + 8, out->buffer_size, &dl);

    if (status != MH3_RES_OK)
        return status;

    if (dl < shape_at || dl - shape_at > shape_size || out->buffer_size > shape_size - (dl - shape_at))
        return MH3_RES_RANGE;

    out->tag_offset = tag;
    out->offset = dl;
    out->commands.data = m->container.data + dl;
    out->commands.size = out->command_size;
    return MH3_RES_OK;
}

mh3_res_status mh3_res_shape_open(const mh3_res_model *m, uint32_t id, mh3_res_shape *out) {
    mh3_res_status status;
    size_t at, parent;
    unsigned i;
    uint32_t size;

    if (!out)
        return MH3_RES_INVALID;

    memset(out, 0, sizeof(*out));
    status = mh3_res_model_entry(m, MH3_RES_SHAPES, id, &out->entry);

    if (status != MH3_RES_OK)
        return status;

    at = out->entry.offset;

    if (!model_fits(m, at, m->version == 11 ? 0x68 : 0x64))
        return MH3_RES_RANGE;

    size = be32(m->container.data + at);

    if (size < (m->version == 11 ? 0x68u : 0x64u) || !model_fits(m, at, size))
        return MH3_RES_RANGE;

    status = mh3_res_relative(m->container, at, at + 4, 16, &parent);

    if (status != MH3_RES_OK || parent != m->offset)
        return MH3_RES_INVALID;

    if ((be32(m->container.data + at + 0x3c) & UINT32_C(0x7fffffff)) != id)
        return MH3_RES_INVALID;

    out->size = size;
    out->flags = be32(m->container.data + at + 0x34);
    out->matrix_id = (int32_t)signed32(be32(m->container.data + at + 8));
    out->vertex_count = be32(m->container.data + at + 0x40);
    out->triangle_count = be32(m->container.data + at + 0x44);

    for (i = 0; i < 12; ++i)
        out->array_ids[i] = signed16(be16(m->container.data + at + 0x48 + i * 2));

    status = display_list(m, at + 0x18, at, size, &out->setup);

    if (status != MH3_RES_OK)
        return status;

    return display_list(m, at + 0x24, at, size, &out->primitives);
}

/* Shape accessors 800999d8/80099a4c/80099abc/80099b74/80099c14. */
mh3_res_status mh3_res_shape_array(const mh3_res_model *m, const mh3_res_shape *s, uint32_t attr, mh3_res_entry *out) {
    mh3_res_kind kind;

    if (!s || !out || attr >= 14)
        return MH3_RES_INVALID;

    memset(out, 0, sizeof(*out));

    if (attr >= 12) {
        int16_t id;

        if (!m || m->version != 11 || !model_fits(m, s->entry.offset + 0x60, 4))
            return MH3_RES_ABSENT;

        id = signed16(be16(m->container.data + s->entry.offset + 0x60 + (attr - 12) * 2));

        if (id == -1)
            return MH3_RES_ABSENT;

        if (id < -1)
            return MH3_RES_INVALID;

        return mh3_res_model_entry(m, attr == 12 ? MH3_RES_FUR_VECTORS : MH3_RES_FUR_POSITIONS, (uint32_t)id, out);
    }

    if (s->array_ids[attr] == -1)
        return MH3_RES_ABSENT;

    if (s->array_ids[attr] < -1)
        return MH3_RES_INVALID;

    kind = attr == 0 ? MH3_RES_POSITIONS : attr == 1 ? MH3_RES_NORMALS : attr < 4 ? MH3_RES_COLORS : MH3_RES_TEXCOORDS;
    return mh3_res_model_entry(m, kind, (uint32_t)s->array_ids[attr], out);
}

/* GetParent 80099464: resource-relative MDL0, never a guest-address cast. */
mh3_res_status mh3_res_shape_parent(mh3_res_span s, size_t at, mh3_res_model *out) {
    size_t parent;
    mh3_res_status status;

    if (!out)
        return MH3_RES_INVALID;

    memset(out, 0, sizeof(*out));

    if (!fits(s, at, 8))
        return MH3_RES_RANGE;

    status = mh3_res_relative(s, at, at + 4, 16, &parent);

    if (status != MH3_RES_OK)
        return status;

    return mh3_res_model_open(s, parent, out);
}

/* Decoding boundary used by G3dDrawOpaXlu 80078b00. Visibility, animation,
 * mutable material overrides and renderer dispatch remain caller responsibilities. */
mh3_res_status mh3_res_draw_next(const mh3_res_model *m, size_t *cursor, mh3_res_draw *out) {
    size_t at;
    mh3_res_entry entry;
    mh3_res_status status;

    if (!cursor || !out)
        return MH3_RES_INVALID;

    memset(out, 0, sizeof(*out));
    at = *cursor;

    if (!model_fits(m, at, 1))
        return MH3_RES_RANGE;

    if (m->container.data[at] == 1) {
        *cursor = at + 1;
        return MH3_RES_END;
    }

    if (m->container.data[at] != 4)
        return MH3_RES_UNSUPPORTED;

    if (!model_fits(m, at, 8))
        return MH3_RES_RANGE;

    out->offset = at;
    out->material_id = be16(m->container.data + at + 1);
    out->shape_id = be16(m->container.data + at + 3);
    out->node_id = be16(m->container.data + at + 5);
    out->priority = m->container.data[at + 7];
    status = mh3_res_model_entry(m, MH3_RES_MATERIALS, out->material_id, &entry);

    if (status != MH3_RES_OK)
        return status;

    status = mh3_res_model_entry(m, MH3_RES_SHAPES, out->shape_id, &entry);

    if (status != MH3_RES_OK)
        return status;

    status = mh3_res_model_entry(m, MH3_RES_NODES, out->node_id, &entry);

    if (status != MH3_RES_OK)
        return status;

    *cursor = at + 8;
    return MH3_RES_OK;
}

/* Submission-span portions of 80099f80 and SubmitResolvedDisplayList 8009a0dc.
 * 80099710 becomes shape.setup.commands directly; no wrapper objects needed. */
mh3_res_status mh3_res_shape_submit_span(const mh3_res_shape *s, int primitive, int same, mh3_res_span *out) {
    if (!s || !out)
        return MH3_RES_INVALID;

    *out = primitive ? s->primitives.commands : s->setup.commands;

    if (!primitive && same) {
        if (out->size < 32) {
            out->data = NULL;
            out->size = 0;
            return MH3_RES_INVALID;
        }

        out->data += 32;
        out->size -= 32;
    }

    return MH3_RES_OK;
}

/* GXSetArray 80099844 and DisableSetArray 80099930. */
static mh3_res_status array_commands(mh3_res_mut_span c, const mh3_res_shape *s, uint32_t attr, size_t *at) {
    mh3_res_span read = {c.data, c.size};
    size_t relative;

    if (!s || !at || attr >= 12)
        return MH3_RES_INVALID;

    relative = 0x32 + (size_t)attr * 12;

    if (relative > s->setup.buffer_size || 12 > s->setup.buffer_size - relative)
        return MH3_RES_RANGE;

    if (s->setup.offset > SIZE_MAX - relative)
        return MH3_RES_RANGE;

    *at = s->setup.offset + relative;
    return fits(read, *at, 12) ? MH3_RES_OK : MH3_RES_RANGE;
}

mh3_res_status mh3_res_shape_set_array(mh3_res_mut_span c, const mh3_res_shape *s, uint32_t attr, uint32_t address, uint8_t stride) {
    size_t at;
    mh3_res_status status = array_commands(c, s, attr, &at);

    if (status != MH3_RES_OK)
        return status;

    c.data[at] = 8;
    c.data[at + 1] = (uint8_t)(0xa0 + attr);
    put32(c.data + at + 2, address);
    c.data[at + 6] = 8;
    c.data[at + 7] = (uint8_t)(0xb0 + attr);
    put32(c.data + at + 8, stride);
    return MH3_RES_OK;
}

mh3_res_status mh3_res_shape_disable_array(mh3_res_mut_span c, const mh3_res_shape *s, uint32_t attr) {
    size_t at;
    mh3_res_status status = array_commands(c, s, attr, &at);

    if (status == MH3_RES_OK)
        memset(c.data + at, 0, 12);

    return status;
}

/* ResTlutObj::Invalidate 8009504c and CopyTo 80095114. The validity-reference
 * assertion in 8009521c is replaced by explicit span/flag validation. */
mh3_res_status mh3_res_tlut_invalidate(mh3_res_mut_span object, uint32_t id) {
    uint32_t flags;

    if (!object.data || object.size < 4)
        return MH3_RES_RANGE;

    if (id >= 8)
        return MH3_RES_RANGE;

    flags = be32(object.data);

    if (flags & UINT32_C(0xffffff00))
        return MH3_RES_INVALID;

    put32(object.data, flags & ~(UINT32_C(1) << id));
    return MH3_RES_OK;
}

mh3_res_status mh3_res_tlut_copy(mh3_res_span source, mh3_res_mut_span destination) {
    uint32_t flags;
    size_t n;
    mh3_res_span dst = {destination.data, destination.size};

    if (!fits(source, 0, 4) || !fits(dst, 0, 4))
        return MH3_RES_RANGE;

    flags = be32(source.data);

    if (flags & UINT32_C(0xffffff00))
        return MH3_RES_INVALID;

    n = !flags ? 4 : !(flags & 0xfc) ? 0x24 : !(flags & 0xe0) ? 0x44 : 0x64;

    if (!fits(source, 0, n) || !fits(dst, 0, n))
        return MH3_RES_RANGE;

    memmove(destination.data, source.data, n);
    return MH3_RES_OK;
}

/* ResTexSrt::CopyTo 8009528c: two header words, then only flagged slots.
 * Full substructures contain native-independent serialized 32-bit float words;
 * copying them avoids all decompiler paired-single/prototype ambiguities. */
mh3_res_status mh3_res_texsrt_copy(mh3_res_span source, mh3_res_mut_span destination) {
    uint32_t flags;
    unsigned i;
    uint8_t copy[0x248];
    mh3_res_span dst = {destination.data, destination.size};

    if (!fits(source, 0, sizeof(copy)) || !fits(dst, 0, sizeof(copy)))
        return MH3_RES_RANGE;
    /* A snapshot makes partially overlapping copies deterministic. */
    memcpy(copy, source.data, sizeof(copy));
    flags = be32(copy);
    memcpy(destination.data, copy, 8);

    for (i = 0; i < 8; ++i) {
        if ((flags >> (i * 4)) & 15) {
            memcpy(destination.data + 8 + i * 20, copy + 8 + i * 20, 20);
            memcpy(destination.data + 0xa8 + i * 52, copy + 0xa8 + i * 52, 52);
        }
    }

    return MH3_RES_OK;
}
