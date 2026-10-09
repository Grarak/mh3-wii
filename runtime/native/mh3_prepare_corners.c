#include "mh3_prepare_corners.h"
#include <math.h>
#include <string.h>

#define NONE UINT32_MAX
enum {
    MAX_STRIDE = 128,
    MAX_VERTICES = 65536,
    INDEX_CHUNK = 4092
};

typedef struct {
    uint8_t semantic, mode, type, components, fraction, bytes;
} attribute;

typedef struct {
    uint32_t cp[256];
    uint8_t known[256];
    attribute attrs[21];
    uint32_t attr_count, corner_stride;
    mh3_prepared_corners result;
    mh3_native_vertex_source arrays[12];
    uint8_t array_known[12];
    const uint8_t *array_data[12];
    uint16_t sources[3][10];
    uint32_t root, source_corners, source_triangles;
    uint8_t index_buffer[INDEX_CHUNK];
    uint32_t buffered;
} decode;

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t bits(float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

static int fits(size_t size, size_t at, size_t n) {
    return at <= size && n <= size - at;
}

typedef struct {
    const void *data;
    size_t bytes;
} region;

static int disjoint(const region *r, unsigned count) {
    for (unsigned i = 0; i < count; i++)
        if (r[i].bytes) {
            const uintptr_t a = (uintptr_t)r[i].data;

            if (!a || r[i].bytes > UINTPTR_MAX - a)
                return 0;

            for (unsigned j = 0; j < i; j++)
                if (r[j].bytes) {
                    const uintptr_t b = (uintptr_t)r[j].data;

                    if (a < b + r[j].bytes && b < a + r[i].bytes)
                        return 0;
                }
        }
    return 1;
}

static int named(mh3_res_span owner, size_t base, size_t field, const char *expected) {
    size_t at;

    if (mh3_res_relative(owner, base, field, 1, &at) != MH3_RES_OK)
        return 0;

    const uint8_t *end = memchr(owner.data + at, 0, owner.size - at);
    const size_t n = end ? (size_t)(end - (owner.data + at)) : 0;
    return end && n == strlen(expected) && !memcmp(owner.data + at, expected, n);
}

static uint32_t height(const mh3_corner_tree_node *nodes, uint32_t n) {
    return n == NONE ? 0 : nodes[n].height;
}

static void reheight(mh3_corner_tree_node *n, mh3_corner_tree_node *all) {
    uint32_t a = height(all, n->left), b = height(all, n->right);
    n->height = 1 + (a > b ? a : b);
}

static uint32_t rotate(mh3_corner_tree_node *nodes, uint32_t n, int left) {
    uint32_t x = left ? nodes[n].right : nodes[n].left;

    if (left) {
        nodes[n].right = nodes[x].left;
        nodes[x].left = n;
    } else {
        nodes[n].left = nodes[x].right;
        nodes[x].right = n;
    }

    reheight(&nodes[n], nodes);
    reheight(&nodes[x], nodes);
    return x;
}

static mh3_corners_status unique(decode *d, const mh3_corner_workspace *w, const uint8_t *value, uint16_t *out) {
    uint32_t path[32], dirs[32], depth = 0, n = d->root, stride = d->result.layout.stride;

    while (n != NONE) {
        int cmp = memcmp(value, w->vertices + (size_t)n * stride, stride);

        if (!cmp) {
            *out = (uint16_t)n;
            return MH3_CORNERS_OK;
        }

        if (depth == 32)
            return MH3_CORNERS_INVALID;

        path[depth] = n;
        dirs[depth++] = cmp > 0;
        n = cmp > 0 ? w->tree[n].right : w->tree[n].left;
    }

    n = d->result.vertex_count;

    if (n >= MAX_VERTICES || n >= w->tree_count || (uint64_t)(n + 1) * stride > w->vertex_bytes)
        return MH3_CORNERS_RANGE;

    memcpy(w->vertices + (size_t)n * stride, value, stride);
    w->tree[n] = (mh3_corner_tree_node){NONE, NONE, 1};

    if (depth) {
        if (dirs[depth - 1])
            w->tree[path[depth - 1]].right = n;
        else
            w->tree[path[depth - 1]].left = n;
    } else
        d->root = n;

    *out = (uint16_t)n;
    d->result.vertex_count++;

    while (depth) {
        depth--;
        uint32_t p = path[depth], r = p;
        reheight(&w->tree[p], w->tree);
        int balance = (int)height(w->tree, w->tree[p].left) - (int)height(w->tree, w->tree[p].right);

        if (balance > 1) {
            uint32_t child = w->tree[p].left;

            if (height(w->tree, w->tree[child].left) < height(w->tree, w->tree[child].right))
                w->tree[p].left = rotate(w->tree, child, 1);

            r = rotate(w->tree, p, 0);
        } else if (balance < -1) {
            uint32_t child = w->tree[p].right;

            if (height(w->tree, w->tree[child].right) < height(w->tree, w->tree[child].left))
                w->tree[p].right = rotate(w->tree, child, 0);

            r = rotate(w->tree, p, 1);
        }

        if (!depth)
            d->root = r;
        else if (dirs[depth - 1])
            w->tree[path[depth - 1]].right = r;
        else
            w->tree[path[depth - 1]].left = r;
    }

    return MH3_CORNERS_OK;
}

static mh3_corners_status append_attr(decode *d, uint8_t semantic, uint8_t mode, uint8_t type, uint8_t components, uint8_t fraction) {
    if (!mode)
        return MH3_CORNERS_OK;

    uint32_t bytes = 1;

    if (semantic >= 9) {
        const unsigned attr = semantic - 9;

        if ((attr == 1 && components != 3) || (attr >= 4 && components > 2))
            return MH3_CORNERS_UNSUPPORTED;

        if (attr == 2 || attr == 3) {
            static const uint8_t color_bytes[6] = {2, 3, 4, 2, 3, 4};

            if (type >= 6)
                return MH3_CORNERS_UNSUPPORTED;

            bytes = color_bytes[type];
        } else {
            static const uint8_t number_bytes[5] = {1, 1, 2, 2, 4};

            if (type >= 5)
                return MH3_CORNERS_UNSUPPORTED;

            bytes = number_bytes[type] * components;
        }

        if (mode >= 2)
            bytes = mode - 1;
    }

    d->attrs[d->attr_count++] = (attribute){semantic, mode, type, components, fraction, (uint8_t)bytes};
    d->corner_stride += bytes;
    return MH3_CORNERS_OK;
}

static mh3_corners_status layout(decode *d, unsigned vat) {
    if (!d->known[0x50] || !d->known[0x60] || !d->known[0x70 + vat] || !d->known[0x80 + vat] || !d->known[0x90 + vat])
        return MH3_CORNERS_INVALID;

    const uint32_t lo = d->cp[0x50], hi = d->cp[0x60], a = d->cp[0x70 + vat], b = d->cp[0x80 + vat], c = d->cp[0x90 + vat];
    d->attr_count = d->corner_stride = 0;

    for (unsigned i = 0; i < 9; i++)
        if ((lo >> i) & 1)
            append_attr(d, (uint8_t)i, 1, 0, 1, 0);
    if ((lo >> 11 & 3) && (a >> 31))
        return MH3_CORNERS_UNSUPPORTED;

    const uint8_t types[12] = {(a >> 1) & 7,  (a >> 10) & 7, (a >> 14) & 7, (a >> 18) & 7, (a >> 22) & 7, (b >> 1) & 7,
                               (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7,  (c >> 15) & 7, (c >> 24) & 7};
    const uint8_t components[12] = {2 + (a & 1),       3 + 6 * (a >> 9 & 1), 4, 4, 1 + (a >> 21 & 1), 1 + (b & 1), 1 + (b >> 9 & 1), 1 + (b >> 18 & 1), 1 + (b >> 27 & 1), 1 + (c >> 5 & 1),
                                    1 + (c >> 14 & 1), 1 + (c >> 23 & 1)};
    const uint8_t fractions[12] = {
        (a >> 4) & 31, types[1] == 1 ? 6 : types[1] == 3 ? 14 : 0, 0, 0, (a >> 25) & 31, (b >> 4) & 31, (b >> 13) & 31, (b >> 22) & 31, c & 31, (c >> 9) & 31, (c >> 18) & 31, (c >> 27) & 31};
    for (unsigned i = 0; i < 12; i++) {
        const uint8_t mode = (uint8_t)(i < 4 ? (lo >> (9 + 2 * i)) & 3 : (hi >> (2 * (i - 4))) & 3);
        mh3_corners_status s = append_attr(d, (uint8_t)(9 + i), mode, types[i], components[i], fractions[i]);

        if (s)
            return s;
    }

    return MH3_CORNERS_OK;
}

static mh3_corners_status arrays(const mh3_res_model *model, const mh3_res_shape *shape, decode *d) {
    for (unsigned i = 0; i < d->attr_count; i++) {
        const attribute *a = &d->attrs[i];

        if (a->semantic < 9 || a->mode < 2)
            continue;

        unsigned id = a->semantic - 9;

        if (!d->array_known[id]) {
            mh3_res_entry e;
            mh3_res_status s = mh3_res_shape_array(model, shape, id, &e);

            if (s)
                return MH3_CORNERS_INVALID;

            if (!fits(model->container.size, e.offset, 32))
                return MH3_CORNERS_RANGE;

            uint32_t size = be32(model->container.data + e.offset);

            if (e.offset < model->offset || !fits(model->size, e.offset - model->offset, size))
                return MH3_CORNERS_RANGE;

            size_t parent;

            if (mh3_res_relative(model->container, e.offset, e.offset + 4, 1, &parent) != MH3_RES_OK || parent != model->offset)
                return MH3_CORNERS_INVALID;

            if (!named(model->container, e.offset, e.offset + 12, e.name))
                return MH3_CORNERS_INVALID;

            mh3_vertex_kind kind = id == 0 ? MH3_VERTEX_POSITION : id == 1 ? MH3_VERTEX_NORMAL : id < 4 ? MH3_VERTEX_COLOR : MH3_VERTEX_UV;

            if (mh3_vertex_source_read((mh3_res_span){model->container.data + e.offset, size}, 0, kind, &d->arrays[id]) != MH3_RES_OK || d->arrays[id].source_id != e.id)
                return MH3_CORNERS_INVALID;

            d->array_data[id] = model->container.data + e.offset;
            d->array_known[id] = 1;
        }

        if (d->arrays[id].format != a->type || d->arrays[id].components != a->components)
            return MH3_CORNERS_INVALID;
    }

    return MH3_CORNERS_OK;
}

static mh3_corners_status output_layout(decode *d, int first) {
    mh3_pack_layout next = {0};
    unsigned mask = 0, at = 0;

    for (unsigned i = 0; i < d->attr_count; i++) {
        attribute *a = &d->attrs[i];

        if (a->semantic < 9) {
            mask |= 1u << a->semantic;
            continue;
        }

        unsigned semantic = a->semantic - 9, components = semantic == 0 ? 3 : a->components;
        next.attributes[next.count++] = (mh3_pack_attribute){(uint16_t)semantic, (uint16_t)at, (uint8_t)((semantic == 2 || semantic == 3) ? 4 : 9), (uint8_t)components};
        at += components * ((semantic == 2 || semantic == 3) ? 1 : 4);
    }

    if (!next.count || next.attributes[0].semantic != 0)
        return MH3_CORNERS_UNSUPPORTED;

    for (unsigned i = 0; i < 3; i++)
        if (mask & (i == 0 ? 15u : i == 1 ? 240u : 256u)) {
            unsigned n = i == 2 ? 1 : 4;
            next.attributes[next.count++] = (mh3_pack_attribute){(uint16_t)(12 + i), (uint16_t)at, 0, (uint8_t)n};
            at += n;
        }
    next.stride = (at + 3) & ~3u;
    next.flags = mask << 8;

    if (next.stride > MAX_STRIDE)
        return MH3_CORNERS_UNSUPPORTED;

    if (!first && memcmp(&next, &d->result.layout, sizeof next))
        return MH3_CORNERS_UNSUPPORTED;

    d->result.layout = next;
    d->result.matrix_mask = mask;
    return MH3_CORNERS_OK;
}

static uint32_t triangle_count(unsigned kind, unsigned count) {
    return kind == 0x90 ? count / 3 : kind == 0x80 ? (count / 4) * 2 + (count % 4 == 3) : count > 2 ? count - 2 : 0;
}

static mh3_corners_status numeric(const uint8_t *p, unsigned type, unsigned count, unsigned fraction, uint8_t *out) {
    for (unsigned i = 0; i < count; i++) {
        float f;
        uint32_t word;

        if (type == 4) {
            word = be32(p);
            memcpy(&f, &word, 4);
            p += 4;
        } else {
            int value = type == 0 ? *p : type == 1 ? (*p < 128 ? *p : (int)*p - 256) : type == 2 ? be16(p) : (be16(p) < 32768 ? be16(p) : (int)be16(p) - 65536);
            p += type < 2 ? 1 : 2;
            f = ldexpf((float)value, -(int)fraction);
            word = bits(f);
        }

        if (!isfinite(f))
            return MH3_CORNERS_INVALID;

        le32(out + 4 * i, word);
    }

    return MH3_CORNERS_OK;
}

static void color(const uint8_t *p, unsigned type, uint8_t *out) {
    uint32_t v = type == 0 || type == 3 ? be16(p) : (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];

    if (type == 0) {
        unsigned r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
        out[0] = (uint8_t)((r << 3) | (r >> 2));
        out[1] = (uint8_t)((g << 2) | (g >> 4));
        out[2] = (uint8_t)((b << 3) | (b >> 2));
        out[3] = 255;
    } else if (type == 1 || type == 2 || type == 5) {
        memcpy(out, p, 3);
        out[3] = type == 5 ? p[3] : 255;
    } else if (type == 3)
        for (unsigned i = 0; i < 4; i++)
            out[i] = (uint8_t)((v >> (12 - 4 * i) & 15) * 17);
    else
        for (unsigned i = 0; i < 4; i++) {
            unsigned n = v >> (18 - 6 * i) & 63;
            out[i] = (uint8_t)((n << 2) | (n >> 4));
        }
}

static mh3_corners_status corner(const uint8_t *p, decode *d, const mh3_corner_workspace *w, uint16_t *out) {
    uint8_t value[MAX_STRIDE] = {0}, selectors[9] = {0};
    unsigned dest = 0;

    for (unsigned i = 0; i < d->attr_count; i++) {
        const attribute *a = &d->attrs[i];
        const uint8_t *source = p;

        if (a->semantic < 9) {
            unsigned selector = *p;

            if (selector % 3 || selector > (a->semantic ? 60u : 27u))
                return MH3_CORNERS_UNSUPPORTED;

            selectors[a->semantic] = (uint8_t)(selector / 3);
            p++;
            continue;
        }

        unsigned id = a->semantic - 9, type = a->type, fraction = a->fraction, components = a->components;

        if (a->mode >= 2) {
            unsigned index = a->mode == 2 ? *p : be16(p);
            const mh3_native_vertex_source *array = &d->arrays[id];

            if (index >= array->count)
                return MH3_CORNERS_RANGE;

            source = d->array_data[id] + array->payload_address + (size_t)index * array->stride;
            type = array->format;
            fraction = array->fraction;
            components = array->components;
        }

        mh3_corners_status s = MH3_CORNERS_OK;

        if (id == 2 || id == 3) {
            color(source, type, value + dest);
            dest += 4;
        } else {
            s = numeric(source, type, components, fraction, value + dest);
            dest += (id == 0 ? 3 : components) * 4;
        }

        if (s)
            return s;

        p += a->bytes;
    }

    for (unsigned i = 0; i < 3; i++)
        if (d->result.matrix_mask & (i == 0 ? 15u : i == 1 ? 240u : 256u)) {
            unsigned n = i == 2 ? 1 : 4;
            memcpy(value + dest, selectors + 4 * i, n);
            dest += n;
        }
    const uint32_t before = d->result.vertex_count;
    mh3_corners_status s = unique(d, w, value, out);

    if (s)
        return s;

    if (d->result.vertex_count != before)
        for (unsigned i = 0; i < 3; i++) {
            uint32_t v = (uint32_t)value[i * 4] | (uint32_t)value[i * 4 + 1] << 8 | (uint32_t)value[i * 4 + 2] << 16 | (uint32_t)value[i * 4 + 3] << 24;
            float f;
            memcpy(&f, &v, 4);

            if (!before || f < d->result.bounds_min[i])
                d->result.bounds_min[i] = f;

            if (!before || f > d->result.bounds_max[i])
                d->result.bounds_max[i] = f;
        }
    return MH3_CORNERS_OK;
}

static mh3_corners_status indexed_load(decode *d, unsigned op, const uint8_t *p) {
    if (!d->result.matrix_mask)
        return MH3_CORNERS_UNSUPPORTED;

    unsigned table = op == 0x20 ? 0 : op == 0x28 ? 1 : op == 0x30 ? 2 : 3;
    unsigned source = be16(p), dest = be16(p + 2), address = dest & 0xfff, count = (dest >> 12) + 1;
    unsigned base = table == 0 ? 0 : table == 1 ? 0x400 : 120, stride = table == 1 ? 9 : 12;

    if (table == 3 || source == 65535 || count != stride || address < base || (address - base) % stride || (address - base) / stride >= 10)
        return MH3_CORNERS_UNSUPPORTED;

    d->sources[table][(address - base) / stride] = (uint16_t)source;
    return MH3_CORNERS_OK;
}

static void required(const decode *d, const mh3_corner_workspace *w, uint16_t index, mh3_pack_palette_batch *b) {
    const mh3_pack_layout *l = &d->result.layout;
    uint8_t selectors[9] = {0};
    const uint8_t *v = w->vertices + (size_t)index * l->stride;
    int normal = 0;

    for (unsigned i = 0; i < l->count; i++) {
        const mh3_pack_attribute *a = &l->attributes[i];

        if (a->semantic == 1)
            normal = 1;

        if (a->semantic >= 12)
            memcpy(selectors + 4 * (a->semantic - 12), v + a->offset, a->components);
    }

    unsigned pn = (d->result.matrix_mask & 1) ? selectors[0] : 0;
    b->position_required |= (uint16_t)(1u << pn);

    if (normal)
        b->normal_required |= (uint16_t)(1u << pn);

    for (unsigned coordinate = 0; coordinate < 8; coordinate++)
        if (d->result.matrix_mask & (2u << coordinate)) {
            unsigned slot = selectors[coordinate + 1];

            if (slot < 10) {
                b->position_required |= (uint16_t)(1u << slot);
                b->texture_position_mask |= (uint8_t)(1u << coordinate);
            } else if (slot < 20)
                b->texture_required |= (uint16_t)(1u << (slot - 10));
            else
                b->texture_identity_mask |= (uint8_t)(1u << coordinate);
        }
}

static mh3_corners_status flush(decode *d, const mh3_corner_workspace *w) {
    if (d->buffered && w->indices.write(w->indices.context, (uint64_t)d->result.index_count * 2 - d->buffered, d->index_buffer, d->buffered))
        return MH3_CORNERS_IO;

    d->buffered = 0;
    return MH3_CORNERS_OK;
}

static mh3_corners_status triangle(decode *d, const mh3_corner_workspace *w, unsigned a, unsigned b, unsigned c, mh3_pack_palette_batch *batch) {
    uint16_t ids[3] = {w->corners[a], w->corners[b], w->corners[c]};

    if (d->result.index_count > (MH3_PREPARE_MAX_GPU - 6) / 2)
        return MH3_CORNERS_RANGE;

    for (unsigned i = 0; i < 3; i++) {
        le16(d->index_buffer + d->buffered + 2 * i, ids[i]);

        if (d->result.matrix_mask)
            required(d, w, ids[i], batch);
    }

    d->buffered += 6;
    d->result.index_count += 3;
    return d->buffered == INDEX_CHUNK ? flush(d, w) : MH3_CORNERS_OK;
}

static mh3_corners_status primitive(decode *d, const mh3_corner_workspace *w, const uint8_t *data, unsigned kind, unsigned count) {
    if (count > w->corner_count)
        return MH3_CORNERS_RANGE;

    for (unsigned i = 0; i < count; i++) {
        mh3_corners_status s = corner(data + (size_t)i * d->corner_stride, d, w, &w->corners[i]);

        if (s)
            return s;
    }

    mh3_pack_palette_batch batch = {0};
    batch.first_index = d->result.index_count;
    mh3_corners_status s = MH3_CORNERS_OK;

    if (kind == 0x90)
        for (unsigned i = 0; i + 2 < count; i += 3) {
            s = triangle(d, w, i, i + 1, i + 2, &batch);

            if (s)
                return s;
        }
    else if (kind == 0x98)
        for (unsigned i = 0; i + 2 < count; i++) {
            s = triangle(d, w, i + (i & 1), i + 1 - (i & 1), i + 2, &batch);

            if (s)
                return s;
        }
    else if (kind == 0xa0)
        for (unsigned i = 1; i + 1 < count; i++) {
            s = triangle(d, w, 0, i, i + 1, &batch);

            if (s)
                return s;
        }
    else {
        for (unsigned i = 0; i + 3 < count; i += 4) {
            s = triangle(d, w, i, i + 1, i + 2, &batch);

            if (s)
                return s;

            s = triangle(d, w, i + 2, i + 3, i, &batch);

            if (s)
                return s;
        }

        if (count % 4 == 3) {
            s = triangle(d, w, count - 3, count - 2, count - 1, &batch);

            if (s)
                return s;
        }
    }

    batch.index_count = d->result.index_count - batch.first_index;

    if (!d->result.matrix_mask || !batch.index_count)
        return MH3_CORNERS_OK;

    uint16_t *tables[3] = {batch.position_source, batch.normal_source, batch.texture_source};
    uint16_t *masks[3] = {&batch.position_known, &batch.normal_known, &batch.texture_known};

    for (unsigned t = 0; t < 3; t++)
        for (unsigned slot = 0; slot < 10; slot++) {
            tables[t][slot] = d->sources[t][slot];

            if (tables[t][slot] != 65535)
                *masks[t] |= (uint16_t)(1u << slot);
        }
    if (d->result.batch_count) {
        mh3_pack_palette_batch *last = &w->batches[d->result.batch_count - 1];

        if (last->position_known == batch.position_known && last->normal_known == batch.normal_known && last->texture_known == batch.texture_known &&
            !memcmp(last->position_source, batch.position_source, sizeof batch.position_source) && !memcmp(last->normal_source, batch.normal_source, sizeof batch.normal_source) &&
            !memcmp(last->texture_source, batch.texture_source, sizeof batch.texture_source)) {
            last->index_count += batch.index_count;
            last->position_required |= batch.position_required;
            last->normal_required |= batch.normal_required;
            last->texture_required |= batch.texture_required;
            last->texture_identity_mask |= batch.texture_identity_mask;
            last->texture_position_mask |= batch.texture_position_mask;
            return MH3_CORNERS_OK;
        }
    }

    if (d->result.batch_count >= w->batch_count || !w->batches)
        return MH3_CORNERS_RANGE;

    w->batches[d->result.batch_count++] = batch;
    return MH3_CORNERS_OK;
}

static mh3_corners_status scan(mh3_res_span span, size_t origin, decode *d, const mh3_res_model *model, const mh3_res_shape *shape, const mh3_corner_workspace *work, int draws, int emit,
                               size_t *error) {
    size_t at = 0;
    int first = d->result.layout.count == 0;

    while (at < span.size) {
        const size_t command = at;
        *error = origin + at;
        unsigned op = span.data[at++];
        mh3_corners_status s = MH3_CORNERS_OK;

        if (!op)
            continue;

        if (op == 8) {
            if (!fits(span.size, at, 5))
                return MH3_CORNERS_RANGE;

            unsigned reg = span.data[at];
            d->cp[reg] = be32(span.data + at + 1);
            d->known[reg] = 1;
            at += 5;
        } else if (op == 0x10) {
            if (!fits(span.size, at, 4))
                return MH3_CORNERS_RANGE;

            size_t n = (size_t)(be16(span.data + at) + 1) * 4;

            if (!fits(span.size, at + 4, n))
                return MH3_CORNERS_RANGE;

            at += 4 + n;
        } else if (op == 0x61) {
            if (!fits(span.size, at, 4))
                return MH3_CORNERS_RANGE;

            at += 4;
        } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
            if (!fits(span.size, at, 4))
                return MH3_CORNERS_RANGE;

            if (draws && emit) {
                s = indexed_load(d, op, span.data + at);

                if (s)
                    return s;
            }

            at += 4;
        } else if (draws && ((op & 0xf8) == 0x80 || (op & 0xf8) == 0x90 || (op & 0xf8) == 0x98 || (op & 0xf8) == 0xa0)) {
            if (!fits(span.size, at, 2))
                return MH3_CORNERS_RANGE;

            unsigned count = be16(span.data + at);
            at += 2;
            s = layout(d, op & 7);

            if (s)
                return s;

            s = arrays(model, shape, d);

            if (s)
                return s;

            s = output_layout(d, first);

            if (s)
                return s;

            first = 0;
            size_t bytes = (size_t)count * d->corner_stride;

            if (!fits(span.size, at, bytes))
                return MH3_CORNERS_RANGE;

            if (emit) {
                s = primitive(d, work, span.data + at, op & 0xf8, count);

                if (s)
                    return s;
            } else {
                d->source_corners += count;
                d->source_triangles += triangle_count(op & 0xf8, count);
            }

            at += bytes;
        } else
            return MH3_CORNERS_UNSUPPORTED;

        if (at <= command)
            return MH3_CORNERS_INVALID;
    }

    return MH3_CORNERS_OK;
}

mh3_corners_status mh3_prepare_shape(const mh3_res_model *input, uint32_t id, const mh3_corner_workspace *work, mh3_prepared_corners *out, size_t *error_offset) {
    if (!input || !work || !out || !work->vertices || !work->tree || !work->corners || !work->indices.write || !work->indices.truncate)
        return MH3_CORNERS_INVALID;

    if (input->container.size > MH3_PREPARE_MAX_METADATA || work->vertex_bytes > MAX_VERTICES * MAX_STRIDE || !work->tree_count || work->tree_count > MAX_VERTICES || !work->corner_count ||
        work->corner_count > MAX_VERTICES || work->batch_count > (MH3_PREPARE_MAX_METADATA - 16) / 96)
        return MH3_CORNERS_RANGE;
    const region regions[] = {{input->container.data, input->container.size},
                              {work->vertices, work->vertex_bytes},
                              {work->tree, (size_t)work->tree_count * sizeof *work->tree},
                              {work->corners, (size_t)work->corner_count * sizeof *work->corners},
                              {work->batches, (size_t)work->batch_count * sizeof *work->batches},
                              {out, sizeof *out},
                              {input, sizeof *input},
                              {work, sizeof *work},
                              {error_offset, error_offset ? sizeof *error_offset : 0}};
    if (!disjoint(regions, sizeof regions / sizeof *regions))
        return MH3_CORNERS_INVALID;

    mh3_res_model model;
    mh3_res_shape shape;
    size_t error = input->offset;

    if (mh3_res_model_open(input->container, input->offset, &model) != MH3_RES_OK || mh3_res_shape_open(&model, id, &shape) != MH3_RES_OK)
        return MH3_CORNERS_INVALID;

    if (!named(model.container, shape.entry.offset, shape.entry.offset + 0x38, shape.entry.name))
        return MH3_CORNERS_INVALID;

    if (model.version == 11 && (be16(model.container.data + shape.entry.offset + 0x60) != 65535 || be16(model.container.data + shape.entry.offset + 0x62) != 65535))
        return MH3_CORNERS_UNSUPPORTED;

    decode first = {0};
    first.root = NONE;
    mh3_corners_status s = scan(shape.setup.commands, shape.setup.offset, &first, &model, &shape, work, 0, 0, &error);

    if (!s)
        s = scan(shape.primitives.commands, shape.primitives.offset, &first, &model, &shape, work, 1, 0, &error);

    if (!s && (!first.result.layout.count || first.source_corners != shape.vertex_count || first.source_triangles != shape.triangle_count || (shape.matrix_id < 0 && !(first.result.matrix_mask & 1))))
        s = MH3_CORNERS_INVALID;

    if (!s && ((uint64_t)first.source_triangles * 6 > MH3_PREPARE_MAX_GPU || !first.source_triangles))
        s = MH3_CORNERS_RANGE;

    if (s) {
        if (error_offset)
            *error_offset = error;

        return s;
    }

    if (work->indices.truncate(work->indices.context, 0))
        return MH3_CORNERS_ROLLBACK;

    decode d = {0};
    d.root = NONE;
    memset(d.sources, 255, sizeof d.sources);
    d.result.layout = first.result.layout;
    d.result.matrix_mask = first.result.matrix_mask;
    s = scan(shape.setup.commands, shape.setup.offset, &d, &model, &shape, work, 0, 1, &error);

    if (!s)
        s = scan(shape.primitives.commands, shape.primitives.offset, &d, &model, &shape, work, 1, 1, &error);

    if (!s)
        s = flush(&d, work);

    if (s) {
        if (error_offset)
            *error_offset = error;

        return work->indices.truncate(work->indices.context, 0) ? MH3_CORNERS_ROLLBACK : s;
    }

    for (unsigned i = 0; i < 3; i++)
        d.result.center[i] = (float)(((double)d.result.bounds_min[i] + d.result.bounds_max[i]) * 0.5);

    *out = d.result;
    return MH3_CORNERS_OK;
}

mh3_corners_status mh3_prepare_palettes(const mh3_prepared_corners *shape, const mh3_corner_workspace *work, uint8_t *data, size_t capacity, size_t *size) {
    if (!shape || !work || !data || !size || !shape->matrix_mask || !shape->batch_count || shape->batch_count > work->batch_count || !work->batches)
        return MH3_CORNERS_INVALID;

    const uint64_t bytes = 16 + (uint64_t)shape->batch_count * 96;

    if (bytes > capacity || bytes > MH3_PREPARE_MAX_METADATA)
        return MH3_CORNERS_RANGE;

    const region regions[] = {{data, (size_t)bytes}, {work->batches, (size_t)shape->batch_count * sizeof *work->batches}, {shape, sizeof *shape}, {work, sizeof *work}, {size, sizeof *size}};

    if (!disjoint(regions, sizeof regions / sizeof *regions))
        return MH3_CORNERS_INVALID;

    memset(data, 0, (size_t)bytes);
    memcpy(data, "M3PL", 4);
    le32(data + 4, 1);
    le32(data + 8, shape->batch_count);
    le32(data + 12, 96);

    for (uint32_t i = 0; i < shape->batch_count; i++) {
        const mh3_pack_palette_batch *b = &work->batches[i];
        uint8_t *p = data + 16 + (size_t)i * 96;
        le32(p, b->first_index);
        le32(p + 4, b->index_count);
        le16(p + 8, b->position_known);
        le16(p + 10, b->normal_known);
        le16(p + 12, b->texture_known);
        le16(p + 14, b->position_required);
        le16(p + 16, b->normal_required);
        le16(p + 18, b->texture_required);
        p[20] = b->texture_identity_mask;
        p[21] = b->texture_position_mask;

        for (unsigned t = 0; t < 3; t++)
            for (unsigned slot = 0; slot < 10; slot++)
                le16(p + 24 + t * 20 + slot * 2, (t == 0 ? b->position_source : t == 1 ? b->normal_source : b->texture_source)[slot]);
    }

    *size = (size_t)bytes;
    return MH3_CORNERS_OK;
}
