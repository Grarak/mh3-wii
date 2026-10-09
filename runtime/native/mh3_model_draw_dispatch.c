#include "mh3_model_draw_dispatch.h"
#include <string.h>

static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t half(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}
static void put(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void put_half(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static float stored_depth(uint32_t bits) {
    /* The authored lfs/stfs record assignment quiets a signaling NaN while
     * retaining sign and payload. Plain structure copies alone do not. */
    float value;
    if ((bits & 0x7fffffffu) > 0x7f800000u)
        bits |= 0x00400000u;
    memcpy(&value, &bits, 4);
    return value;
}
static int fits(mh3_res_span span, size_t at, size_t bytes) {
    return span.data && at <= span.size && bytes <= span.size - at;
}
static int resource_fits(const mh3_res_model *model, size_t at, size_t bytes) {
    return model && at >= model->offset && at - model->offset <= model->size && bytes <= model->size - (at - model->offset) && fits(model->container, at, bytes);
}
static mh3_res_status draw_entry(const mh3_res_model *model, mh3_res_kind kind, uint16_t id, mh3_res_entry *out) {
    size_t offset;
    const mh3_res_status status = mh3_res_model_data(model, kind, id, &offset);
    if (status)
        return status;
    out->id = id;
    out->offset = offset;
    out->name = NULL;
    return MH3_RES_OK;
}
static mh3_res_status collect_next(const mh3_res_model *model, size_t *cursor, mh3_res_draw *out) {
    const size_t at = *cursor;
    if (!resource_fits(model, at, 1))
        return MH3_RES_RANGE;
    const uint8_t *record = model->container.data + at;
    if (record[0] == 1) {
        *cursor = at + 1;
        return MH3_RES_END;
    }
    if (record[0] != 4)
        return MH3_RES_UNSUPPORTED;
    if (!resource_fits(model, at, 8))
        return MH3_RES_RANGE;
    out->material_id = half(record + 1);
    out->shape_id = half(record + 3);
    out->node_id = half(record + 5);
    out->priority = record[7];
    out->offset = at;
    *cursor = at + 8;
    return MH3_RES_OK;
}

mh3_res_status mh3_model_draw_record_read(mh3_res_span span, mh3_model_draw_record *out) {
    uint32_t bits;
    if (!out)
        return MH3_RES_INVALID;
    if (!fits(span, 0, 12))
        return MH3_RES_RANGE;
    bits = word(span.data);
    memcpy(&out->depth, &bits, 4);
    out->priority = half(span.data + 4);
    out->node = half(span.data + 6);
    out->material = half(span.data + 8);
    out->shape = half(span.data + 10);
    return MH3_RES_OK;
}
mh3_res_status mh3_model_draw_record_write(mh3_res_mut_span span, const mh3_model_draw_record *record) {
    uint32_t bits;
    if (!record)
        return MH3_RES_INVALID;
    if (!span.data || span.size < 12)
        return MH3_RES_RANGE;
    memcpy(&bits, &record->depth, 4);
    put(span.data, bits);
    put_half(span.data + 4, record->priority);
    put_half(span.data + 6, record->node);
    put_half(span.data + 8, record->material);
    put_half(span.data + 10, record->shape);
    return MH3_RES_OK;
}
void mh3_model_draw_swap(mh3_model_draw_record *a, mh3_model_draw_record *b) {
    mh3_model_draw_record saved = *a;
    *a = *b;
    *b = saved;
    uint32_t bits;
    memcpy(&bits, &a->depth, 4);
    a->depth = stored_depth(bits);
    memcpy(&bits, &b->depth, 4);
    b->depth = stored_depth(bits);
}
int mh3_model_draw_less(const mh3_model_draw_record *a, const mh3_model_draw_record *b, int xlu) {
    if (a->priority < b->priority)
        return 1;
    if (b->priority < a->priority)
        return 0;
    if (!xlu)
        return b->depth < a->depth;
    if (a->depth < b->depth)
        return 1;
    return a->depth == b->depth && a->material < b->material;
}
size_t mh3_model_draw_minimum(const mh3_model_sort_ops *ops, size_t begin, size_t end) {
    size_t best = begin;
    for (size_t i = begin + 1; i < end; i++)
        if (ops->less(ops->user, i, best))
            best = i;
    return best;
}
void mh3_model_draw_selection_sort(const mh3_model_sort_ops *ops, size_t begin, size_t end) {
    for (size_t i = begin; i + 1 < end; i++) {
        size_t best = mh3_model_draw_minimum(ops, i, end);
        if (best != i)
            ops->swap(ops->user, best, i);
    }
}
void mh3_model_draw_median(const mh3_model_sort_ops *ops, size_t a, size_t b, size_t pivot) {
    const int pivot_less_a = ops->less(ops->user, pivot, a);
    const int b_less_pivot = ops->less(ops->user, b, pivot);
    if (pivot_less_a || b_less_pivot) {
        if (pivot_less_a && b_less_pivot)
            ops->swap(ops->user, a, b);
        else {
            if (ops->less(ops->user, b, a))
                ops->swap(ops->user, a, b);
            ops->swap(ops->user, pivot_less_a ? a : b, pivot);
        }
    }
}
static int32_t next_phase(int32_t phase) {
    const uint32_t bits = (uint32_t)phase + 1u;
    int32_t next;
    memcpy(&next, &bits, 4);
    return next > 4 ? -4 : next;
}
static mh3_res_status sort_range(const mh3_model_sort_ops *ops, size_t begin, size_t end, unsigned which) {
    while (end - begin >= 2) {
        size_t count = end - begin;
        if (count < 21) {
            mh3_model_draw_selection_sort(ops, begin, end);
            return MH3_RES_OK;
        }
        int32_t phase = ops->phase(ops->user, which);
        const int32_t second = next_phase(phase);
        ops->set_phase(ops->user, which, next_phase(second));
        const size_t a = begin + (size_t)((int32_t)(count / 4) + phase % 5);
        const size_t b = begin + (size_t)((int32_t)(count * 3 / 4) + second % 5);
        const size_t pivot = end - 1;
        mh3_model_draw_median(ops, a, b, pivot);
        size_t left = begin, right = pivot;
        while (ops->less(ops->user, left, pivot))
            ++left;
        do {
            --right;
            if (left == right)
                break;
        } while (!ops->less(ops->user, right, pivot));
        if (left < right) {
            ops->swap(ops->user, left, right);
            ++left;
            for (;;) {
                while (ops->less(ops->user, left, pivot))
                    ++left;
                do {
                    --right;
                } while (!ops->less(ops->user, right, pivot));
                if (right <= left)
                    break;
                ops->swap(ops->user, left, right);
                ++left;
            }
        }
        if (left == begin) {
            ops->swap(ops->user, left, pivot);
            ++left;
            right = end - 1;
            if (!ops->less(ops->user, begin, right)) {
                while (left != end && !ops->less(ops->user, begin, left))
                    ++left;
                if (left < right)
                    ops->swap(ops->user, left, right);
            }
            if (left < right)
                for (;;) {
                    while (!ops->less(ops->user, begin, left))
                        ++left;
                    do {
                        --right;
                    } while (ops->less(ops->user, begin, right));
                    if (right <= left)
                        break;
                    ops->swap(ops->user, left, right);
                    ++left;
                }
            begin = left;
        } else {
            mh3_res_status status;
            if (left - begin < end - left) {
                status = sort_range(ops, begin, left, 1);
                begin = left;
            } else {
                status = sort_range(ops, left, end, 1);
                end = left;
            }
            if (status)
                return status;
        }
    }
    return MH3_RES_OK;
}
mh3_res_status mh3_model_draw_sort(const mh3_model_sort_ops *ops, size_t begin, size_t end, unsigned phase) {
    if (!ops || !ops->less || !ops->swap || !ops->phase || !ops->set_phase || phase > 1)
        return MH3_RES_INVALID;
    if (begin > end || end > MH3_MODEL_SORT_CAPACITY)
        return MH3_RES_RANGE;
    return sort_range(ops, begin, end, phase);
}

static mh3_res_status visible(const mh3_model_draw_collection *input, const mh3_res_entry *node, int *out) {
    if (!resource_fits(input->model, node->offset, 0x18))
        return MH3_RES_RANGE;
    const uint8_t *bytes = input->model->container.data + node->offset;
    if (input->replacement.data) {
        uint32_t pointer;
        mh3_res_memory_view view;
        if (input->replacement.size < 8)
            return MH3_RES_RANGE;
        pointer = word(input->replacement.data + 4);
        if (pointer) {
            const uint32_t id = word(bytes + 0xc);
            if (id > UINT32_MAX - pointer)
                return MH3_RES_RANGE;
            mh3_res_status status = mh3_res_runtime_map(input->memory, pointer + id, 1, &view);
            if (status)
                return status;
            *out = view.span.data[view.offset] != 0;
            return MH3_RES_OK;
        }
    }
    *out = (word(bytes + 0x14) & 0x100u) != 0;
    return MH3_RES_OK;
}
mh3_res_status mh3_model_draw_collect(const mh3_model_draw_collection *input, mh3_model_draw_record *records, size_t capacity, uint32_t *count) {
    size_t cursor;
    uint32_t used = 0;
    if (!input || !input->model || !records || !count)
        return MH3_RES_INVALID;
    uint32_t missing_bits;
    memcpy(&missing_bits, &input->missing_depth, 4);
    const float missing_depth = stored_depth(missing_bits);
    *count = 0;
    cursor = input->bytecode;
    for (;;) {
        mh3_res_draw draw;
        mh3_res_entry node;
        int show;
        mh3_res_status status = collect_next(input->model, &cursor, &draw);
        if (status == MH3_RES_END)
            break;
        if (status)
            return status;
        status = draw_entry(input->model, MH3_RES_NODES, draw.node_id, &node);
        if (status)
            return status;
        status = visible(input, &node, &show);
        if (status)
            return status;
        if (!show)
            continue;
        if (used >= capacity || used >= MH3_MODEL_SORT_CAPACITY - 1)
            return MH3_RES_RANGE;
        mh3_model_draw_record *record = &records[used++];
        record->node = draw.node_id;
        record->material = draw.material_id;
        record->shape = draw.shape_id;
        record->priority = draw.priority;
        const uint32_t matrix = word(input->model->container.data + node.offset + 0x10);
        record->depth = missing_depth;
        if (matrix < input->positions.count) {
            const mh3_matrix_table *positions = &input->positions;
            if (positions->stride < 48 || matrix > SIZE_MAX / positions->stride)
                return MH3_RES_RANGE;
            const size_t base = (size_t)matrix * positions->stride;
            if (base > SIZE_MAX - 44)
                return MH3_RES_RANGE;
            const size_t at = base + 44;
            if (!fits(positions->bytes, at, 4))
                return MH3_RES_RANGE;
            uint32_t bits;
            if (positions->big_endian)
                bits = word(positions->bytes.data + at);
            else
                memcpy(&bits, positions->bytes.data + at, 4);
            record->depth = stored_depth(bits);
        }
    }
    *count = used;
    return MH3_RES_OK;
}
mh3_res_status mh3_model_draw_sorted_read(const mh3_res_model *model, uint32_t count, uint32_t options, mh3_model_record_read read, void *reader, mh3_model_draw_emit emit, void *user,
                                          mh3_model_draw_stats *stats) {
    mh3_model_draw_stats totals = {0};
    size_t previous = 0;
    int has_previous = 0;
    if (stats)
        *stats = totals;
    if (!model || !read || !emit || count >= MH3_MODEL_SORT_CAPACITY)
        return MH3_RES_INVALID;
    for (uint32_t i = 0; i < count; i++) {
        mh3_model_draw_record value;
        mh3_res_status status = read(reader, i, &value);
        if (status)
            return status;
        const mh3_model_draw_record *record = &value;
        mh3_model_draw_item item = {0};
        ++totals.records;
        status = draw_entry(model, MH3_RES_SHAPES, record->shape, &item.shape);
        if (status)
            return status;
        if (!resource_fits(model, item.shape.offset, 0x38))
            return MH3_RES_RANGE;
        if (word(model->container.data + item.shape.offset + 0x34) & 2u) {
            ++totals.hidden_shapes;
            continue;
        }
        status = draw_entry(model, MH3_RES_MATERIALS, record->material, &item.material);
        if (status)
            return status;
        status = draw_entry(model, MH3_RES_NODES, record->node, &item.node);
        if (status)
            return status;
        if (!resource_fits(model, item.material.offset, 0x10))
            return MH3_RES_RANGE;
        item.draw.material_id = record->material;
        item.draw.shape_id = record->shape;
        item.draw.node_id = record->node;
        item.draw.priority = (uint8_t)record->priority;
        item.material_index = word(model->container.data + item.material.offset + 0xc);
        item.dispatch_flags = 1u | (options & 8u) | ((options & 4u) ? 16u : 0u);
        if (has_previous && previous == item.material.offset) {
            item.dispatch_flags |= 16u;
            ++totals.repeated_materials;
        }
        status = emit(user, &item);
        if (status)
            return status;
        previous = item.material.offset;
        has_previous = 1;
        ++totals.submitted;
    }
    if (stats)
        *stats = totals;
    return MH3_RES_OK;
}
static mh3_res_status array_record(void *reader, uint32_t index, mh3_model_draw_record *out) {
    *out = ((const mh3_model_draw_record *)reader)[index];
    return MH3_RES_OK;
}
mh3_res_status mh3_model_draw_sorted(const mh3_res_model *model, const mh3_model_draw_record *records, uint32_t count, uint32_t options, mh3_model_draw_emit emit, void *user,
                                     mh3_model_draw_stats *stats) {
    if (!records && count)
        return MH3_RES_INVALID;
    return mh3_model_draw_sorted_read(model, count, options, array_record, (void *)records, emit, user, stats);
}

mh3_res_status mh3_model_draw_pipeline_run(const mh3_model_draw_pipeline *ops, const uint32_t sources[3], uint32_t opaque, uint32_t translucent, uint32_t replacement, uint32_t options) {
    mh3_res_status status;
    const uint32_t absent[3] = {0, 0, 0};
    if (!ops || !ops->bind_sources || !ops->pass || !sources || !sources[0])
        return MH3_RES_INVALID;
    ops->bind_sources(ops->user, sources);
    if (!(options & 16u) && ops->complete_sources)
        ops->complete_sources(ops->user);
    if (opaque) {
        status = ops->pass(ops->user, opaque, replacement, options, (options & 1u) != 0, 0);
        if (status)
            return status;
    }
    if (translucent) {
        status = ops->pass(ops->user, translucent, replacement, options, (options & 2u) != 0, 1);
        if (status)
            return status;
    }
    ops->bind_sources(ops->user, absent);
    return MH3_RES_OK;
}
mh3_res_status mh3_model_scene_options(mh3_res_span owner, uint32_t *out) {
    if (!out)
        return MH3_RES_INVALID;
    if (!fits(owner, 0x118, 4))
        return MH3_RES_RANGE;
    *out = word(owner.data + 0x118);
    return MH3_RES_OK;
}
mh3_res_status mh3_model_scene_draw(const mh3_model_scene_draw_ops *ops, mh3_res_span owner, uint32_t identity, uint32_t options) {
    if (!ops || !ops->draw_task)
        return MH3_RES_INVALID;
    if (options == UINT32_MAX) {
        mh3_res_status status = mh3_model_scene_options(owner, &options);
        if (status)
            return status;
    }
    ops->draw_task(ops->user, identity, 6, &options);
    ops->draw_task(ops->user, identity, 7, &options);
    return MH3_RES_OK;
}
mh3_res_status mh3_model_scene_task(const mh3_model_scene_draw_ops *ops, mh3_res_span owner, uint32_t identity, uint32_t task, uint32_t argument, uint32_t options) {
    if (!ops || !ops->handler || !ops->virtual_handler)
        return MH3_RES_INVALID;
    if (!fits(owner, 0xcc, 4))
        return MH3_RES_RANGE;
    if (task >= 1 && task <= 8 && (word(owner.data + 0xcc) & (1u << (task - 1))))
        return MH3_RES_OK;
    if (task == 0x10001) {
        ops->virtual_handler(ops->user, identity, 0x38, options);
        return MH3_RES_OK;
    }
    ops->handler(ops->user, identity, task, argument, options);
    if (task == 8) {
        if (!fits(owner, 0x138, 4))
            return MH3_RES_RANGE;
        const uint32_t child = word(owner.data + 0x138);
        if (child)
            ops->virtual_handler(ops->user, child, 0x24, 0);
    }
    return MH3_RES_OK;
}
/* 8007B9A4 (opaque: flag 0x10, slot 0x18) and 8007B948 (translucent: flag
 * 0x20, slot 0x1c) gate the owner callback on its live flags and mask. */
static mh3_res_status scene_callback(const mh3_model_scene_draw_ops *ops, mh3_res_span owner, uint32_t identity, int translucent, unsigned mask, uint32_t argument, uint32_t options) {
    if (!fits(owner, 0xd4, 8))
        return MH3_RES_RANGE;
    const uint32_t callback = word(owner.data + 0xd4);
    if (callback && (half(owner.data + 0xda) & (translucent ? 0x20u : 0x10u)) && (owner.data[0xd8] & mask)) {
        if (!ops->callback)
            return MH3_RES_INVALID;
        ops->callback(ops->user, callback, translucent ? 0x1c : 0x18, mask, identity, argument, options);
    }
    return MH3_RES_OK;
}
/* 8007DC40 (opaque) and 8007DD1C (translucent) differ in their callback gate,
 * the bytecode stream and the pipeline pass they feed. */
static mh3_res_status scene_pass(const mh3_model_scene_draw_ops *ops, mh3_res_span owner, uint32_t identity, uint32_t argument, uint32_t options_pointer, int translucent) {
    uint32_t options, sources[3];
    mh3_res_status status;
    if (!ops || !ops->source || !ops->draw || !fits(owner, 0x110, 8) || identity > UINT32_MAX - 0x144)
        return MH3_RES_INVALID;
    status = scene_callback(ops, owner, identity, translucent, 1, argument, options_pointer);
    if (status)
        return status;
    if (options_pointer) {
        if (!ops->options)
            return MH3_RES_INVALID;
        options = ops->options(ops->user, options_pointer);
    } else {
        status = mh3_model_scene_options(owner, &options);
        if (status)
            return status;
    }
    if (!fits(owner, 0xe8, 4))
        return MH3_RES_RANGE;
    const uint32_t model = word(owner.data + 0xe8), stream = word(owner.data + (translucent ? 0x114 : 0x110));
    sources[2] = ops->source(ops->user, identity, 2);
    sources[1] = ops->source(ops->user, identity, 1);
    sources[0] = ops->source(ops->user, identity, 0);
    status = ops->draw(ops->user, model, sources, translucent ? 0 : stream, translucent ? stream : 0, identity + 0x144, options);
    if (status)
        return status;
    return scene_callback(ops, owner, identity, translucent, 4, argument, options_pointer);
}
mh3_res_status mh3_model_scene_opa(const mh3_model_scene_draw_ops *ops, mh3_res_span owner, uint32_t identity, uint32_t argument, uint32_t options_pointer) {
    return scene_pass(ops, owner, identity, argument, options_pointer, 0);
}
mh3_res_status mh3_model_scene_xlu(const mh3_model_scene_draw_ops *ops, mh3_res_span owner, uint32_t identity, uint32_t argument, uint32_t options_pointer) {
    return scene_pass(ops, owner, identity, argument, options_pointer, 1);
}
