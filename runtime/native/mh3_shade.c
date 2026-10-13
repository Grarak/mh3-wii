#include "mh3_shade.h"
#include <assert.h>
#include <math.h>
#include <string.h>

/* TEV colour arguments: 0 cprev, 1 aprev, 2 c0, 3 a0, 4 c1, 5 a1, 6 c2, 7 a2,
 * 8 texc, 9 texa, 10 rasc, 11 rasa, 12 one, 13 half, 14 konst, 15 zero.
 * Alpha arguments: 0 aprev, 1 a0, 2 a1, 3 a2, 4 texa, 5 rasa, 6 konst, 7 zero. */
enum {
    COLOR_PREV = 0,
    COLOR_TEXTURE = 8,
    COLOR_TEXTURE_ALPHA = 9,
    COLOR_RASTER = 10,
    COLOR_RASTER_ALPHA = 11,
    COLOR_ONE = 12,
    COLOR_HALF = 13,
    COLOR_KONST = 14,
    COLOR_ZERO = 15,
    ALPHA_PREV = 0,
    ALPHA_TEXTURE = 4,
    ALPHA_RASTER = 5,
    ALPHA_KONST = 6,
    ALPHA_ZERO = 7
};

static mh3_shade_constant constant(uint8_t kind, uint8_t index, uint8_t alpha) {
    mh3_shade_constant c = {{kind, MH3_SHADE_CONSTANT_ONE}, {index, 0}, {alpha, 0}};
    return c;
}

static mh3_shade_term constant_term(mh3_shade_constant value) {
    mh3_shade_term t;
    memset(&t, 0, sizeof t);
    t.offset = value;
    t.scale = constant(MH3_SHADE_CONSTANT_ZERO, 0, 0);
    return t;
}

static mh3_shade_term zero_term(void) {
    return constant_term(constant(MH3_SHADE_CONSTANT_ZERO, 0, 0));
}

static mh3_shade_term source_term(uint8_t source, uint8_t texture, uint8_t replicate_alpha) {
    mh3_shade_term t;
    memset(&t, 0, sizeof t);
    t.source = source;
    t.texture = texture;
    t.replicate_alpha = replicate_alpha;
    t.offset = constant(MH3_SHADE_CONSTANT_ZERO, 0, 0);
    t.scale = constant(MH3_SHADE_CONSTANT_ONE, 0, 0);
    return t;
}

static int is_zero(const mh3_shade_term *t) {
    return t->source == MH3_SHADE_SOURCE_NONE && (t->offset.kind[0] == MH3_SHADE_CONSTANT_ZERO || t->offset.kind[1] == MH3_SHADE_CONSTANT_ZERO);
}

static int is_constant(const mh3_shade_term *t) {
    return t->source == MH3_SHADE_SOURCE_NONE;
}

static int raster_zero(const mh3_pack_tev_stage *stage) {
    return stage->raster_channel == 6 || stage->raster_channel > 8;
}

/* Multiply a constant into a term's free factor slots; 0 when out of slots. */
static int merge(mh3_shade_term *t, const mh3_shade_constant *value) {
    mh3_shade_constant *target = is_constant(t) ? &t->offset : &t->scale;
    mh3_shade_constant next = *target;

    for (unsigned i = 0; i < 2; ++i) {
        if (value->kind[i] == MH3_SHADE_CONSTANT_ONE)
            continue;

        unsigned slot = 0;

        while (slot < 2 && next.kind[slot] != MH3_SHADE_CONSTANT_ONE)
            ++slot;

        if (slot == 2)
            return 0;

        next.kind[slot] = value->kind[i];
        next.index[slot] = value->index[i];
        next.alpha[slot] = value->alpha[i];
    }

    if (!is_constant(t) && t->offset.kind[0] != MH3_SHADE_CONSTANT_ZERO)
        return 0; /* offset + scale * source: a factor would have to scale both */
    *target = next;
    return 1;
}

/* x * y as one term (one side constant). */
static int product(const mh3_shade_term *x, const mh3_shade_term *y, mh3_shade_term *out) {
    if (is_constant(y)) {
        *out = *x;
        return merge(out, &y->offset);
    }

    if (is_constant(x)) {
        *out = *y;
        return merge(out, &x->offset);
    }

    return 0;
}

typedef struct {
    mh3_shade_material *out;
    const mh3_native_material_state *state;
    uint8_t quality;
    unsigned stage;    /* stage whose arguments are being mapped */
    uint8_t exhausted; /* a stage sampled a map beyond the three roles */
    uint8_t extended;  /* also fold mix(A, prev, C) and the raster product */
} classifier;

/* Texture role for a stage's (map, coordinate); allocates the next role. */
static int texture_role(classifier *c, const mh3_pack_tev_stage *stage) {
    mh3_shade_material *m = c->out;

    for (unsigned i = 0; i < m->textures; ++i)
        if (m->texture_map[i] == stage->texture_map && m->texture_coordinate[i] == stage->texture_coordinate)
            return (int)i;
    if (m->textures >= 3 || stage->texture_map == 255)
        return -1;

    m->texture_map[m->textures] = stage->texture_map;
    m->texture_coordinate[m->textures] = stage->texture_coordinate;
    return m->textures++;
}

static uint8_t role(classifier *c, const mh3_pack_tev_stage *stage) {
    const int r = texture_role(c, stage);

    if (r < 0) {
        c->quality |= MH3_SHADE_FALLBACK;

        if (stage->texture_map != 255)
            c->exhausted = 1;

        return 0;
    }

    return (uint8_t)r;
}

static void note_swap(classifier *c, const mh3_pack_tev_stage *stage, int texture) {
    const mh3_pack_swap_table *swap = &c->state->swaps[texture ? stage->texture_swap & 3 : stage->raster_swap & 3];

    if ((swap->known & 15) == 15 && (swap->rgba[0] != 0 || swap->rgba[1] != 1 || swap->rgba[2] != 2 || swap->rgba[3] != 3))
        c->quality |= MH3_SHADE_SWAP;
}

/* Registers are material constants only until a stage writes them. */
static void note_register(classifier *c, unsigned index, int alpha) {
    for (unsigned i = 0; i < c->stage; ++i) {
        const mh3_pack_tev_pass *p = alpha ? &c->state->tev[i].alpha : &c->state->tev[i].color;

        if (p->output == index)
            c->quality |= MH3_SHADE_FALLBACK;
    }
}

static mh3_shade_term color_argument(classifier *c, uint8_t argument) {
    const mh3_pack_tev_stage *stage = &c->state->tev[c->stage];

    switch (argument) {
    case COLOR_TEXTURE:
    case COLOR_TEXTURE_ALPHA:
        note_swap(c, stage, 1);
        return source_term(MH3_SHADE_SOURCE_TEXTURE, role(c, stage), argument == COLOR_TEXTURE_ALPHA);
    case COLOR_RASTER:
    case COLOR_RASTER_ALPHA:
        if (raster_zero(stage))
            return zero_term();

        note_swap(c, stage, 0);

        if (stage->raster_channel == 1 || stage->raster_channel == 3 || stage->raster_channel == 5)
            c->quality |= MH3_SHADE_CHANNEL1;

        if (stage->raster_channel == 7 || stage->raster_channel == 8)
            c->quality |= MH3_SHADE_INDIRECT;

        return source_term(MH3_SHADE_SOURCE_VERTEX, 0, argument == COLOR_RASTER_ALPHA);
    case COLOR_ONE:
        return constant_term(constant(MH3_SHADE_CONSTANT_ONE, 0, 0));
    case COLOR_HALF:
        return constant_term(constant(MH3_SHADE_CONSTANT_HALF, 0, 0));
    case COLOR_KONST:
        return constant_term(constant(MH3_SHADE_CONSTANT_KONST, stage->konst_color, 0));
    case COLOR_ZERO:
        return zero_term();
    default:
        /* 0..7: register colour (even) or replicated register alpha (odd). */
        note_register(c, argument / 2u, argument & 1);
        return constant_term(constant(MH3_SHADE_CONSTANT_REGISTER, (uint8_t)(argument / 2), (uint8_t)(argument & 1)));
    }
}

static mh3_shade_term alpha_argument(classifier *c, uint8_t argument) {
    const mh3_pack_tev_stage *stage = &c->state->tev[c->stage];

    switch (argument) {
    case ALPHA_TEXTURE:
        note_swap(c, stage, 1);
        return source_term(MH3_SHADE_SOURCE_TEXTURE, role(c, stage), 0);
    case ALPHA_RASTER:
        if (raster_zero(stage))
            return zero_term();

        note_swap(c, stage, 0);
        return source_term(MH3_SHADE_SOURCE_VERTEX, 0, 0);
    case ALPHA_KONST:
        return constant_term(constant(MH3_SHADE_CONSTANT_KONST, (uint8_t)(stage->konst_alpha | 0x80), 1));
    case ALPHA_ZERO:
        return zero_term();
    default:
        note_register(c, argument, 1);
        return constant_term(constant(MH3_SHADE_CONSTANT_REGISTER, argument, 1));
    }
}

static mh3_shade_term argument(classifier *c, int alpha, uint8_t value) {
    return alpha ? alpha_argument(c, value) : color_argument(c, value);
}

static const mh3_pack_tev_pass *pass_of(const classifier *c, unsigned stage, int alpha) {
    return alpha ? &c->state->tev[stage].alpha : &c->state->tev[stage].color;
}

static int is_prev(uint8_t value, int alpha) {
    return alpha ? value == ALPHA_PREV : value == COLOR_PREV;
}

static int is_zero_argument(uint8_t value, int alpha) {
    return value == (alpha ? ALPHA_ZERO : COLOR_ZERO);
}

static int reads_prev(const mh3_pack_tev_pass *p, int alpha) {
    return is_prev(p->a, alpha) || is_prev(p->b, alpha) || is_prev(p->c, alpha);
}

/* d + 0, prev untouched: mix(a, b, c) vanishes when a is zero or c is one
 * and b is zero or c is zero. */
static int passthrough(const mh3_pack_tev_pass *p, int alpha) {
    const int c_one = !alpha && p->c == COLOR_ONE, c_zero = is_zero_argument(p->c, alpha);
    return is_prev(p->d, alpha) && p->operation == 0 && p->bias == 0 && p->scale == 0 && (is_zero_argument(p->a, alpha) || c_one) && (is_zero_argument(p->b, alpha) || c_zero);
}

/* eq == b * c (a = d = 0, no bias, unsigned). */
static int product_form(const mh3_shade_equation *eq) {
    return is_zero(&eq->input[0]) && is_zero(&eq->input[3]) && !eq->bias && !eq->subtract;
}

static void stage_equation(classifier *c, int alpha, mh3_shade_equation *out) {
    const mh3_pack_tev_pass *p = pass_of(c, c->stage, alpha);
    const uint8_t arguments[4] = {p->a, p->b, p->c, p->d};

    for (unsigned i = 0; i < 4; ++i)
        out->input[i] = argument(c, alpha, arguments[i]);

    if (p->operation >= 2) {
        c->quality |= MH3_SHADE_COMPARE;
        out->subtract = out->bias = out->scale = 0;
    } else {
        out->subtract = p->operation;
        out->bias = p->bias;
        out->scale = p->scale;
    }
}

/* prev * x: multiply x into whichever of b/c can take it. */
static int fold_multiply(classifier *c, const mh3_pack_tev_pass *p, int alpha, mh3_shade_equation *eq) {
    if (p->operation || p->bias || !is_zero_argument(p->a, alpha) || !is_zero_argument(p->d, alpha))
        return 0;

    uint8_t other;

    if (is_prev(p->b, alpha) && !is_prev(p->c, alpha))
        other = p->c;
    else if (is_prev(p->c, alpha) && !is_prev(p->b, alpha))
        other = p->b;
    else
        return 0;

    if (eq->scale && p->scale)
        return 0;
    /* mix(A, B, s) * x with constant A and B is mix(A x, B x, s): NW4R's
     * colour-mapped text, mix(c0, c1, tex) * ras (CharWriter::SetupGX
     * with a colour mapping, 0x80503C38). */
    if (!product_form(eq)) {
        if (!is_zero(&eq->input[3]) || eq->bias || eq->subtract || !is_constant(&eq->input[0]) || !is_constant(&eq->input[1]) || is_constant(&eq->input[2]))
            return 0;

        const mh3_shade_term x = argument(c, alpha, other);
        mh3_shade_term a, b;

        if (!product(&eq->input[0], &x, &a) || !product(&eq->input[1], &x, &b))
            return 0;

        eq->input[0] = a;
        eq->input[1] = b;

        if (p->scale)
            eq->scale = p->scale;

        c->quality |= MH3_SHADE_FOLDED;
        return 1;
    }

    const mh3_shade_term x = argument(c, alpha, other);

    for (unsigned slot = 1; slot <= 2; ++slot) {
        mh3_shade_term merged;

        if (product(&eq->input[slot], &x, &merged)) {
            eq->input[slot] = merged;

            if (p->scale)
                eq->scale = p->scale;

            c->quality |= MH3_SHADE_FOLDED;
            return 1;
        }
    }

    return 0;
}

/* prev + mix(a, b, c): the current product moves into d. A third layer of
 * the form prev + b * c becomes d + mix(2 * x, 2 * y, 1/2). */
static int fold_layer(classifier *c, const mh3_pack_tev_pass *p, int alpha, mh3_shade_equation *eq) {
    if (!is_prev(p->d, alpha) || reads_prev(p, alpha) || p->operation >= 2 || eq->bias || eq->subtract || eq->scale)
        return 0;

    mh3_shade_equation next;
    const uint8_t arguments[3] = {p->a, p->b, p->c};

    for (unsigned i = 0; i < 3; ++i)
        next.input[i] = argument(c, alpha, arguments[i]);

    if (is_zero(&eq->input[3])) {
        if (!product_form(eq) || !product(&eq->input[1], &eq->input[2], &next.input[3]))
            return 0;
    } else {
        /* third layer: both existing and new mixes must be single products */
        const mh3_shade_constant two = constant(MH3_SHADE_CONSTANT_FRACTION, 16, 0);
        mh3_shade_term x, y;

        if (!is_zero(&eq->input[0]) || !is_zero(&next.input[0]) || p->operation || !product(&eq->input[1], &eq->input[2], &x) || !product(&next.input[1], &next.input[2], &y) || !merge(&x, &two) ||
            !merge(&y, &two))
            return 0;
        next.input[0] = x;
        next.input[1] = y;
        next.input[2] = constant_term(constant(MH3_SHADE_CONSTANT_HALF, 0, 0));
        next.input[3] = eq->input[3];
    }

    next.subtract = p->operation;
    next.bias = p->bias;
    next.scale = p->scale;
    *eq = next;
    c->quality |= MH3_SHADE_FOLDED;
    return 1;
}

/* mix(A, prev, C) or mix(prev, B, C) over a single product prev: prev
 * collapses into one term (texture or vertex times constants). */
static int fold_mix(classifier *c, const mh3_pack_tev_pass *p, int alpha, mh3_shade_equation *eq) {
    if (!c->extended || p->operation >= 2 || is_prev(p->c, alpha) || is_prev(p->d, alpha) || is_prev(p->a, alpha) == is_prev(p->b, alpha) || !product_form(eq) || eq->scale)
        return 0;

    mh3_shade_term prev;

    if (!product(&eq->input[1], &eq->input[2], &prev))
        return 0;

    mh3_shade_equation next;
    const uint8_t arguments[4] = {p->a, p->b, p->c, p->d};

    for (unsigned i = 0; i < 4; ++i)
        next.input[i] = is_prev(arguments[i], alpha) ? prev : argument(c, alpha, arguments[i]);

    next.subtract = p->operation;
    next.bias = p->bias;
    next.scale = p->scale;
    *eq = next;
    c->quality |= MH3_SHADE_FOLDED;
    return 1;
}

/* A last stage prev * ras over a clamped prev: BASIC's raster product. */
static int fold_raster(classifier *c, const mh3_pack_tev_pass *p, int alpha) {
    if (!c->extended || c->stage + 1 != c->state->tev_count || !c->stage || !pass_of(c, c->stage - 1, alpha)->clamp)
        return 0;

    if (p->operation || p->bias || p->scale || !is_zero_argument(p->a, alpha) || !is_zero_argument(p->d, alpha) || is_prev(p->b, alpha) == is_prev(p->c, alpha))
        return 0;

    const uint8_t other = is_prev(p->b, alpha) ? p->c : p->b;

    if (other != (alpha ? ALPHA_RASTER : COLOR_RASTER))
        return 0;

    const mh3_shade_term x = argument(c, alpha, other);

    if (x.source != MH3_SHADE_SOURCE_VERTEX)
        return 0;

    c->out->raster_product |= (uint8_t)(1u << alpha);
    return 1;
}

/* Fold one channel's stages start..last into eq. */
static void fold_channel(classifier *c, int alpha, unsigned start, unsigned count, mh3_shade_equation *eq) {
    if (pass_of(c, start, alpha)->output != 0 && start + 1 != count)
        c->quality |= MH3_SHADE_FALLBACK;

    c->stage = start;
    stage_equation(c, alpha, eq);

    for (unsigned i = start + 1; i < count; ++i) {
        const mh3_pack_tev_pass *p = pass_of(c, i, alpha);
        c->stage = i;
        /* A non-final stage writing another register leaves prev alone; reads
         * of that register later are caught by note_register. */
        if ((p->output != 0 && i + 1 != count) || passthrough(p, alpha))
            continue;

        if (!fold_multiply(c, p, alpha, eq) && !fold_layer(c, p, alpha, eq) && !fold_mix(c, p, alpha, eq) && !fold_raster(c, p, alpha))
            c->quality |= MH3_SHADE_FALLBACK;
    }
}

/* Base + masked overlay, then lit (the authored four-stage family):
 *   s0: reg2 = texture0 * k0          s1: prev = texture1 * k1 + reg2
 *   s2: prev = mix(reg2, prev, tex2)  s3: prev = prev * ras (optional)  */
static int overlay(classifier *c) {
    const mh3_native_material_state *s = c->state;

    if (s->tev_count != 3 && s->tev_count != 4)
        return 0;

    const mh3_pack_tev_pass *p0 = &s->tev[0].color, *p1 = &s->tev[1].color, *p2 = &s->tev[2].color;

    if (p0->output != 3 || p0->a != COLOR_ZERO || p0->b != COLOR_TEXTURE || p0->d != COLOR_ZERO || p0->operation || p0->bias || p0->scale)
        return 0;

    if (p1->a != COLOR_ZERO || p1->b != COLOR_TEXTURE || p1->d != 6 || p1->operation || p1->bias || p1->output)
        return 0;

    if (p2->a != 6 || p2->b != COLOR_PREV || p2->c != COLOR_TEXTURE || p2->d != COLOR_ZERO || p2->operation || p2->bias || (p2->output && s->tev_count == 4))
        return 0;

    mh3_shade_material *m = c->out;

    if (texture_role(c, &s->tev[0]) != 0 || texture_role(c, &s->tev[1]) != 1 || texture_role(c, &s->tev[2]) != 2)
        return 0;

    for (unsigned i = 0; i < 3; ++i)
        note_swap(c, &s->tev[i], 1);
    /* colour: (texture0 * k0 + overlay * t1 * t2) * C * scale */
    mh3_shade_equation *eq = &m->color;
    eq->input[0] = eq->input[3] = zero_term();
    c->stage = 0;
    eq->input[1] = source_term(MH3_SHADE_SOURCE_TEXTURE, 0, 0);
    eq->input[2] = color_argument(c, p0->c);
    c->stage = 1;
    const mh3_shade_term k1 = color_argument(c, p1->c);
    eq->subtract = eq->bias = 0;
    eq->scale = p2->scale;

    if (!is_constant(&k1)) {
        /* (t0 + t1 * t2) * ras when both stages take the same raster factor. */
        const mh3_shade_term *k0 = &eq->input[2];

        if (k0->source != k1.source || k0->replicate_alpha != k1.replicate_alpha || s->tev[0].raster_channel != s->tev[1].raster_channel)
            return 0;

        m->overlay_scale = constant(MH3_SHADE_CONSTANT_ONE, 0, 0);
    } else {
        if (k1.offset.kind[1] != MH3_SHADE_CONSTANT_ONE)
            return 0;

        m->overlay_scale = k1.offset;
        /* The overlay is added to b, so before stage 3 c must be one: a
         * constant k0 folds into b; a raster k0 would also scale the overlay. */
        if (is_constant(&eq->input[2])) {
            mh3_shade_term base = eq->input[1];

            if (merge(&base, &eq->input[2].offset)) {
                eq->input[1] = base;
                eq->input[2] = constant_term(constant(MH3_SHADE_CONSTANT_ONE, 0, 0));
            }
        }

        if (!is_constant(&eq->input[2]) || eq->input[2].offset.kind[0] != MH3_SHADE_CONSTANT_ONE)
            c->quality |= MH3_SHADE_FALLBACK;
    }

    if (p1->scale) {
        /* (t1 * k1 + reg2) * s blended by t2: keep the overlay's scale, drop
         * the scaled reg2 * t2 remainder. */
        static const uint8_t eighths[4] = {8, 16, 32, 4};
        m->overlay_scale.kind[1] = MH3_SHADE_CONSTANT_FRACTION;
        m->overlay_scale.index[1] = eighths[p1->scale & 3];
        m->overlay_scale.alpha[1] = 0;
        c->quality |= MH3_SHADE_FALLBACK;
    }

    if (s->tev_count == 4) {
        c->stage = 3;

        if (!passthrough(&s->tev[3].color, 0) && !fold_multiply(c, &s->tev[3].color, 0, eq))
            return 0;
    }
    /* Alpha folds from the first start stage that is exact with the roles the
     * overlay holds. When none is and some start needs a fourth texture (the
     * em024 wings: a last stage writes a0 = map3.a * ras.a), drop the overlay
     * so the base texture and the alpha map share the three roles. */
    unsigned start = 0;
    int exact = 0, exhausted = 0;

    for (unsigned i = 0; i < s->tev_count && !exact; ++i) {
        mh3_shade_material trial = *m;
        classifier t = {&trial, s, 0, 0, 0, 0};
        fold_channel(&t, 1, i, s->tev_count, &trial.alpha);
        exhausted |= t.exhausted;

        if (!(t.quality & (MH3_SHADE_FALLBACK | MH3_SHADE_COMPARE))) {
            start = i;
            exact = 1;
        }
    }

    if (!exact && exhausted)
        return 0;

    fold_channel(c, 1, start, s->tev_count, &m->alpha);
    m->overlay = 1;
    return 1;
}

/* MODULATE: a = 0, b * c with one factor a texture role (or constant) and the
 * other the vertex colour (or constant), d constant. Overlay bases use role 0
 * only (roles 1/2 feed the overlay); other bases may use roles 0 and 1. */
static int texture_side(const mh3_shade_material *m, const mh3_shade_term *t, int alpha) {
    if (is_constant(t))
        return 1;

    return t->source == MH3_SHADE_SOURCE_TEXTURE && (alpha || !t->replicate_alpha) && (t->texture == 0 || (t->texture == 1 && !m->overlay));
}

static int vertex_side(const mh3_shade_term *t, int alpha) {
    return is_constant(t) || (t->source == MH3_SHADE_SOURCE_VERTEX && (alpha || !t->replicate_alpha));
}

static int modulate_channel(const mh3_shade_material *m, const mh3_shade_equation *e, int alpha) {
    if ((m->textures > 2 && !m->overlay) || !is_zero(&e->input[0]) || !is_constant(&e->input[3]))
        return 0;

    return (texture_side(m, &e->input[1], alpha) && vertex_side(&e->input[2], alpha)) || (vertex_side(&e->input[1], alpha) && texture_side(m, &e->input[2], alpha));
}

static int literal_constant(const mh3_shade_constant *c, uint8_t kind) {
    return c->kind[0] == kind && c->kind[1] == MH3_SHADE_CONSTANT_ONE;
}

static int literal_zero_term(const mh3_shade_term *t) {
    return is_constant(t) && literal_constant(&t->offset, MH3_SHADE_CONSTANT_ZERO);
}

static int unscaled_source(const mh3_shade_term *t, uint8_t source) {
    return t->source == source && t->texture == 0 && !t->replicate_alpha &&
           literal_constant(&t->offset, MH3_SHADE_CONSTANT_ZERO) && literal_constant(&t->scale, MH3_SHADE_CONSTANT_ONE);
}

static int pure_modulate_channel(const mh3_shade_equation *e) {
    return !e->subtract && !e->bias && !e->scale && literal_zero_term(&e->input[0]) && literal_zero_term(&e->input[3]) &&
           ((unscaled_source(&e->input[1], MH3_SHADE_SOURCE_TEXTURE) && unscaled_source(&e->input[2], MH3_SHADE_SOURCE_VERTEX)) ||
            (unscaled_source(&e->input[2], MH3_SHADE_SOURCE_TEXTURE) && unscaled_source(&e->input[1], MH3_SHADE_SOURCE_VERTEX)));
}

static int alpha_always(const mh3_pack_pixel_inputs *pixel) {
    const uint32_t c0 = pixel->values[MH3_PIXEL_ALPHA_COMPARE0], c1 = pixel->values[MH3_PIXEL_ALPHA_COMPARE1], op = pixel->values[MH3_PIXEL_ALPHA_OPERATION];

    if (op == 0)
        return c0 == 7 && c1 == 7;

    if (op == 1)
        return c0 == 7 || c1 == 7;

    return 0;
}

static void reset(mh3_shade_material *out, const mh3_native_material_state *state) {
    memset(out, 0, sizeof *out);
    out->overlay_scale = constant(MH3_SHADE_CONSTANT_ZERO, 0, 0);
    out->alpha_test = !alpha_always(&state->pixel);
}

/* LAYERS: each channel is a sum of terms, each a texture role, the vertex
 * colour or a constant times a constant, then at most one multiply by the
 * vertex colour or a constant. Per term slot (role 0..2, 3 without a
 * texture) the vertex and the constant coefficients accumulate. */
typedef struct {
    mh3_shade_constant kv[4], kc[4];
    mh3_shade_term factor;
    uint8_t scale;
} layer_sum;

/* A static constant in eighths (zero, one, half, fractions); 0 when a factor
 * is a register or konst value. */
static int static_eighths(const mh3_shade_constant *k, unsigned *out) {
    unsigned value = 8;

    for (unsigned f = 0; f < 2; ++f) {
        switch (k->kind[f]) {
        case MH3_SHADE_CONSTANT_ZERO:
            *out = 0;
            return 1;
        case MH3_SHADE_CONSTANT_ONE:
            break;
        case MH3_SHADE_CONSTANT_HALF:
            value *= 4;
            break;
        case MH3_SHADE_CONSTANT_FRACTION:
            value *= k->index[f];
            break;
        default:
            return 0;
        }

        if (k->kind[f] != MH3_SHADE_CONSTANT_ONE) {
            if (value % 8)
                return 0;

            value /= 8;
        }
    }

    *out = value;
    return 1;
}

/* slot += k: into an empty slot, or two static constants summed. */
static int add_constant(mh3_shade_constant *slot, const mh3_shade_constant *k) {
    unsigned a, b;

    if (static_eighths(k, &b) && !b)
        return 1;

    if (static_eighths(slot, &a) && !a) {
        *slot = *k;
        return 1;
    }

    if (!static_eighths(slot, &a) || !static_eighths(k, &b) || a + b > 255)
        return 0;

    *slot = constant(MH3_SHADE_CONSTANT_FRACTION, (uint8_t)(a + b), 0);
    return 1;
}

/* The constant factor of an argument term: a constant's value, a source's scale. */
static int factor_of(const mh3_shade_term *t, mh3_shade_constant *out) {
    if (is_constant(t)) {
        *out = t->offset;
        return 1;
    }

    if (t->offset.kind[0] != MH3_SHADE_CONSTANT_ZERO && t->offset.kind[1] != MH3_SHADE_CONSTANT_ZERO)
        return 0;

    *out = t->scale;
    return 1;
}

/* 1 - k for a constant of one factor; 0 when k has two. */
static int complement(const mh3_shade_constant *k, mh3_shade_constant *out) {
    unsigned n;

    if (static_eighths(k, &n)) {
        if (n > 8)
            return 0;

        *out = constant(MH3_SHADE_CONSTANT_FRACTION, (uint8_t)(8 - n), 0);
        return 1;
    }

    const unsigned f = k->kind[0] == MH3_SHADE_CONSTANT_ONE ? 1 : 0;

    if (k->kind[1 - f] != MH3_SHADE_CONSTANT_ONE || (k->kind[f] != MH3_SHADE_CONSTANT_REGISTER && k->kind[f] != MH3_SHADE_CONSTANT_KONST))
        return 0;

    *out = constant(k->kind[f] == MH3_SHADE_CONSTANT_KONST ? MH3_SHADE_CONSTANT_KONST_COMPLEMENT : MH3_SHADE_CONSTANT_REGISTER_COMPLEMENT, k->index[f], k->alpha[f]);
    return 1;
}

/* sum += x * y. Colour terms take colours, not replicated alphas. */
static int layer_add(layer_sum *l, mh3_shade_term x, mh3_shade_term y, int alpha) {
    if (is_zero(&x) || is_zero(&y))
        return 1;

    if (y.source == MH3_SHADE_SOURCE_TEXTURE) {
        const mh3_shade_term t = x;
        x = y;
        y = t;
    }

    if (y.source == MH3_SHADE_SOURCE_TEXTURE || (x.source == MH3_SHADE_SOURCE_VERTEX && y.source == MH3_SHADE_SOURCE_VERTEX) || (!alpha && (x.replicate_alpha || y.replicate_alpha)))
        return 0;

    mh3_shade_constant kx, ky;

    if (!factor_of(&x, &kx) || !factor_of(&y, &ky))
        return 0;

    mh3_shade_term k = constant_term(kx);

    if (!merge(&k, &ky))
        return 0;

    const unsigned slot = x.source == MH3_SHADE_SOURCE_TEXTURE ? x.texture : 3;
    const int vertex = x.source == MH3_SHADE_SOURCE_VERTEX || y.source == MH3_SHADE_SOURCE_VERTEX;
    return add_constant(vertex ? &l->kv[slot] : &l->kc[slot], &k.offset);
}

static int layer_channel(classifier *c, int alpha, unsigned count, layer_sum *l) {
    const mh3_shade_term one = constant_term(constant(MH3_SHADE_CONSTANT_ONE, 0, 0));
    int started = 0, closed = 0;
    unsigned last = 0;

    for (unsigned i = 0; i < 4; ++i)
        l->kv[i] = l->kc[i] = constant(MH3_SHADE_CONSTANT_ZERO, 0, 0);

    l->factor = one;
    l->scale = 0;

    for (unsigned i = 0; i < count; ++i) {
        const mh3_pack_tev_pass *p = pass_of(c, i, alpha);
        c->stage = i;

        if (p->output != 0)
            return 0;

        if (started && passthrough(p, alpha))
            continue;

        if (closed || p->operation || p->bias)
            return 0;

        /* prev * x ends the sum; the sum is saturated (its last stage clamps). */
        if (started && is_zero_argument(p->a, alpha) && is_zero_argument(p->d, alpha) && is_prev(p->b, alpha) != is_prev(p->c, alpha)) {
            mh3_shade_term x = argument(c, alpha, is_prev(p->b, alpha) ? p->c : p->b);

            if (!pass_of(c, last, alpha)->clamp || x.source == MH3_SHADE_SOURCE_TEXTURE || (!alpha && x.replicate_alpha))
                return 0;

            /* Its scale (x2, x4, x1/2) goes into the factor. */
            static const uint8_t eighths[4] = {8, 16, 32, 4};
            const mh3_shade_constant scale = constant(MH3_SHADE_CONSTANT_FRACTION, eighths[p->scale & 3], 0);

            if (p->scale && !merge(&x, &scale))
                return 0;

            l->factor = x;
            closed = 1;
            continue;
        }

        /* A scaled stage scales the sum so far: only the last may scale. */
        if (reads_prev(p, alpha) || is_prev(p->d, alpha) != started || l->scale)
            return 0;

        if (!started && !layer_add(l, argument(c, alpha, p->d), one, alpha))
            return 0;

        /* mix(a, b, c) = a (1 - c) + b c */
        if (is_zero_argument(p->c, alpha)) {
            if (!layer_add(l, argument(c, alpha, p->a), one, alpha))
                return 0;
        } else if (!alpha && p->c == COLOR_ONE) {
            if (!layer_add(l, argument(c, alpha, p->b), one, alpha))
                return 0;
        } else if (is_zero_argument(p->b, alpha)) {
            const mh3_shade_term weight = argument(c, alpha, p->c);
            mh3_shade_constant rest;

            if (!is_constant(&weight) || !complement(&weight.offset, &rest) || !layer_add(l, argument(c, alpha, p->a), constant_term(rest), alpha))
                return 0;
        } else if (!is_zero_argument(p->a, alpha) || !layer_add(l, argument(c, alpha, p->b), argument(c, alpha, p->c), alpha))
            return 0;

        l->scale = p->scale;
        started = 1;
        last = i;
    }

    return started;
}

static mh3_shade_term layer_term(const mh3_shade_constant *kv, const mh3_shade_constant *kc) {
    unsigned v;

    if (static_eighths(kv, &v) && !v)
        return constant_term(*kc);

    mh3_shade_term t = source_term(MH3_SHADE_SOURCE_VERTEX, 0, 0);
    t.offset = *kc;
    t.scale = *kv;
    return t;
}

static int layers(classifier *c, unsigned count) {
    mh3_shade_material *m = c->out;

    for (int alpha = 0; alpha < 2; ++alpha) {
        layer_sum l;

        if (!layer_channel(c, alpha, count, &l))
            return 0;

        mh3_shade_equation *eq = alpha ? &m->alpha : &m->color;

        for (unsigned i = 0; i < 4; ++i)
            eq->input[i] = layer_term(&l.kv[i], &l.kc[i]);

        eq->subtract = eq->bias = 0;
        eq->scale = l.scale;
        m->factor[alpha] = l.factor;
    }

    return !(c->quality & (MH3_SHADE_FALLBACK | MH3_SHADE_COMPARE));
}

/* One classification: the folds, else LAYERS. extended adds the folds that
 * only replace a fallback (mh3_shade_classify). */
static uint8_t classify_pass(const mh3_native_material_state *state, mh3_shade_material *out, int extended, int *layered) {
    reset(out, state);
    classifier c = {out, state, 0, 0, 0, 0};
    const unsigned count = state->tev_count ? (state->tev_count < 16 ? state->tev_count : 16) : 1;

    if (!overlay(&c)) {
        c.extended = (uint8_t)extended;
        /* Try each start stage per channel; keep the first exact fold, else
         * the first stage that samples a texture. Textures are re-allocated
         * by the final attempt so roles only cover what is read. */
        unsigned start[2] = {0, 0};

        for (int alpha = 0; alpha < 2; ++alpha) {
            unsigned chosen = UINT32_MAX, textured = UINT32_MAX;

            for (unsigned s = 0; s < count && chosen == UINT32_MAX; ++s) {
                const mh3_pack_tev_pass *p = pass_of(&c, s, alpha);
                const uint8_t texture = alpha ? ALPHA_TEXTURE : COLOR_TEXTURE;

                if (textured == UINT32_MAX && (p->a == texture || p->b == texture || p->c == texture || p->d == texture ||
                                               (!alpha && (p->a == COLOR_TEXTURE_ALPHA || p->b == COLOR_TEXTURE_ALPHA || p->c == COLOR_TEXTURE_ALPHA || p->d == COLOR_TEXTURE_ALPHA))))
                    textured = s;
                mh3_shade_material trial;
                reset(&trial, state);
                classifier t = {&trial, state, 0, 0, 0, (uint8_t)extended};
                fold_channel(&t, alpha, s, count, alpha ? &trial.alpha : &trial.color);

                if (!(t.quality & (MH3_SHADE_FALLBACK | MH3_SHADE_COMPARE)))
                    chosen = s;
            }

            start[alpha] = chosen != UINT32_MAX ? chosen : textured != UINT32_MAX ? textured : 0;
        }

        reset(out, state);
        c.quality = 0;
        fold_channel(&c, 0, start[0], count, &out->color);
        fold_channel(&c, 1, start[1], count, &out->alpha);

        if (c.quality & (MH3_SHADE_FALLBACK | MH3_SHADE_COMPARE)) {
            mh3_shade_material trial;
            reset(&trial, state);
            classifier t = {&trial, state, 0, 0, 0, 0};

            if (layers(&t, count)) {
                *out = trial;
                c.quality = t.quality;
                *layered = 1;
            }
        }
    }
    /* The raster product multiplies colour and alpha together. */
    if (out->raster_product && out->raster_product != 3) {
        out->raster_product = 0;
        c.quality |= MH3_SHADE_FALLBACK;
    }

    return c.quality;
}

uint8_t mh3_shade_classify(const mh3_native_material_state *state, mh3_shade_material *out) {
    const unsigned count = state->tev_count ? (state->tev_count < 16 ? state->tev_count : 16) : 1;
    int layered = 0;
    uint8_t quality = classify_pass(state, out, 0, &layered);

    if (quality & (MH3_SHADE_FALLBACK | MH3_SHADE_COMPARE)) {
        mh3_shade_material trial;
        int trial_layered = 0;
        const uint8_t extended = classify_pass(state, &trial, 1, &trial_layered);

        if (!(extended & (MH3_SHADE_FALLBACK | MH3_SHADE_COMPARE))) {
            *out = trial;
            quality = extended;
            layered = trial_layered;
        }
    }

    for (unsigned i = 0; i < count; ++i)
        if (state->tev[i].indirect_matrix)
            quality |= MH3_SHADE_INDIRECT;
    if (!(quality & (MH3_SHADE_SWAP | MH3_SHADE_FALLBACK | MH3_SHADE_COMPARE | MH3_SHADE_INDIRECT | MH3_SHADE_CHANNEL1)))
        quality |= MH3_SHADE_EXACT;

    out->quality = quality;
    out->model = layered ? MH3_SHADE_LAYERS : !out->raster_product && modulate_channel(out, &out->color, 0) && modulate_channel(out, &out->alpha, 1) ? MH3_SHADE_MODULATE : MH3_SHADE_BASIC;
    out->pure_modulate = (quality & MH3_SHADE_EXACT) && out->model == MH3_SHADE_MODULATE && out->textures == 1 && !out->overlay &&
                         pure_modulate_channel(&out->color) && pure_modulate_channel(&out->alpha);
    return quality;
}

/* ---- Per-draw resolution ---- */
#define UNIT_BYTE(b) ((float)(b) / 255.f)
#define UNIT_BYTE4(b) UNIT_BYTE(b), UNIT_BYTE((b) + 1), UNIT_BYTE((b) + 2), UNIT_BYTE((b) + 3)
#define UNIT_BYTE16(b) UNIT_BYTE4(b), UNIT_BYTE4((b) + 4), UNIT_BYTE4((b) + 8), UNIT_BYTE4((b) + 12)
#define UNIT_BYTE64(b) UNIT_BYTE16(b), UNIT_BYTE16((b) + 16), UNIT_BYTE16((b) + 32), UNIT_BYTE16((b) + 48)

const float mh3_unit_byte[256] = {UNIT_BYTE64(0), UNIT_BYTE64(64), UNIT_BYTE64(128), UNIT_BYTE64(192)};

static void register_value(const mh3_native_material_state *s, unsigned index, float out[4]) {
    const mh3_pack_tev_color *color = &s->color[index & 3];

    for (unsigned i = 0; i < 4; ++i)
        out[i] = mh3_unit_value(color->rgba[i]);
}

static void konst_value(const mh3_native_material_state *s, uint8_t selector, float out[4]) {
    const int alpha_selector = (selector & 0x80) != 0;
    selector &= 0x7f;

    if (selector < 8) {
        const float fraction = selector ? (8 - selector) / 8.f : 1.f;
        out[0] = out[1] = out[2] = out[3] = fraction;
        return;
    }

    if (!alpha_selector && selector >= 12 && selector < 16) {
        const mh3_pack_tev_color *k = &s->konst[selector - 12];

        for (unsigned i = 0; i < 3; ++i)
            out[i] = mh3_unit_value(k->rgba[i]);

        out[3] = mh3_unit_value(k->rgba[3]);
        return;
    }

    if (selector >= 16 && selector < 32) {
        const mh3_pack_tev_color *k = &s->konst[(selector - 16) & 3];
        const float v = mh3_unit_value(k->rgba[(selector - 16) / 4]);
        out[0] = out[1] = out[2] = out[3] = v;
        return;
    }

    out[0] = out[1] = out[2] = out[3] = 0.f;
}

static void factor(const mh3_native_material_state *s, uint8_t kind, uint8_t index, uint8_t alpha, float out[4]) {
    switch (kind) {
    case MH3_SHADE_CONSTANT_ZERO:
        out[0] = out[1] = out[2] = out[3] = 0.f;
        return;
    case MH3_SHADE_CONSTANT_ONE:
        out[0] = out[1] = out[2] = out[3] = 1.f;
        return;
    case MH3_SHADE_CONSTANT_HALF:
        out[0] = out[1] = out[2] = out[3] = 0.5f;
        return;
    case MH3_SHADE_CONSTANT_REGISTER:
        register_value(s, index, out);
        break;
    case MH3_SHADE_CONSTANT_KONST:
        konst_value(s, index, out);
        break;
    case MH3_SHADE_CONSTANT_REGISTER_COMPLEMENT:
    case MH3_SHADE_CONSTANT_KONST_COMPLEMENT:
        factor(s, kind == MH3_SHADE_CONSTANT_KONST_COMPLEMENT ? MH3_SHADE_CONSTANT_KONST : MH3_SHADE_CONSTANT_REGISTER, index, alpha, out);

        for (unsigned i = 0; i < 4; ++i)
            out[i] = 1.f - out[i];

        return;
    default:
        out[0] = out[1] = out[2] = out[3] = index / 8.f;
        return;
    }

    if (alpha)
        out[0] = out[1] = out[2] = out[3];
}

static void resolve(const mh3_native_material_state *s, const mh3_shade_constant *c, float out[4]) {
    float second[4];
    factor(s, c->kind[0], c->index[0], c->alpha[0], out);
    factor(s, c->kind[1], c->index[1], c->alpha[1], second);

    for (unsigned i = 0; i < 4; ++i)
        out[i] *= second[i];
}

static void term(const mh3_native_material_state *s, const mh3_shade_term *t, mh3_shade_input *out) {
    resolve(s, &t->offset, out->offset);
    resolve(s, &t->scale, out->scale);

    for (unsigned i = 0; i < 4; ++i)
        out->select[i] = out->swizzle[i] = 0.f;

    if (t->source == MH3_SHADE_SOURCE_TEXTURE)
        out->select[t->texture < 3 ? t->texture : 0] = 1.f;
    else if (t->source == MH3_SHADE_SOURCE_VERTEX)
        out->select[3] = 1.f;

    out->swizzle[0] = t->replicate_alpha ? 1.f : 0.f;
}

static float equation_scale(uint8_t scale);
static float equation_bias(uint8_t bias);
static void equation(const mh3_shade_equation *e, float out[4]) {
    out[0] = equation_bias(e->bias);
    out[1] = equation_scale(e->scale);
    out[2] = e->subtract ? -1.f : 1.f;
    out[3] = 0.f;
}

/* GX alpha compare as an inclusive integer interval; NEQUAL keeps the larger
 * side (exact at 0 and 255). */
static void compare_interval(uint32_t function, int reference, int *lo, int *hi) {
    *lo = 0;
    *hi = 255;

    switch (function) {
    case 0:
        *lo = 256;
        break;
    case 1:
        *hi = reference - 1;
        break;
    case 2:
        *lo = *hi = reference;
        break;
    case 3:
        *hi = reference;
        break;
    case 4:
        *lo = reference + 1;
        break;
    case 5:
        if (reference >= 128)
            *hi = reference - 1;
        else
            *lo = reference + 1;

        break;
    case 6:
        *lo = reference;
        break;
    default:
        break;
    }
}

static void alpha_range(const uint32_t *pixel, float out[4]) {
    int lo0, hi0, lo1, hi1, lo, hi;
    compare_interval(pixel[MH3_PIXEL_ALPHA_COMPARE0], (int)pixel[MH3_PIXEL_ALPHA_REFERENCE0], &lo0, &hi0);
    compare_interval(pixel[MH3_PIXEL_ALPHA_COMPARE1], (int)pixel[MH3_PIXEL_ALPHA_REFERENCE1], &lo1, &hi1);
    const int empty0 = lo0 > hi0, empty1 = lo1 > hi1;

    switch (pixel[MH3_PIXEL_ALPHA_OPERATION]) {
    case 0: /* AND */
        lo = lo0 > lo1 ? lo0 : lo1;
        hi = hi0 < hi1 ? hi0 : hi1;
        break;
    case 1: /* OR: hull (exact when the intervals touch or one is empty) */
        if (empty0 || empty1) {
            lo = empty0 ? lo1 : lo0;
            hi = empty0 ? hi1 : hi0;
        } else {
            lo = lo0 < lo1 ? lo0 : lo1;
            hi = hi0 > hi1 ? hi0 : hi1;
        }

        break;
    default: /* XOR/XNOR: the first comparison */
        lo = lo0;
        hi = hi0;
        break;
    }
    /* integer alpha v = floor(a * 255 + 0.5): v >= lo <=> a >= (lo - 0.5) / 255 */
    out[0] = lo <= 0 ? -1.f : (lo - 0.5f) / 255.f;
    out[1] = hi >= 255 ? 2.f : (hi + 0.5f) / 255.f;
    out[2] = out[3] = 0.f;
}

static float equation_scale(uint8_t scale) {
    static const float values[4] = {1.f, 2.f, 4.f, 0.5f};
    return values[scale & 3];
}

static float equation_bias(uint8_t bias) {
    return bias == 1 ? 128.f / 255.f : bias == 2 ? -128.f / 255.f : 0.f;
}

static void modulate_fill(const mh3_shade_material *m, const mh3_native_material_state *s, mh3_shade_modulate *out) {
    memset(out, 0, sizeof *out);

    for (int alpha = 0; alpha < 2; ++alpha) {
        const mh3_shade_equation *e = alpha ? &m->alpha : &m->color;
        const unsigned first = alpha ? 3 : 0, last = alpha ? 4 : 3;
        const int swapped = !(texture_side(m, &e->input[1], alpha) && vertex_side(&e->input[2], alpha));
        const mh3_shade_term *base = &e->input[swapped ? 2 : 1], *light = &e->input[swapped ? 1 : 2];
        float offset[4], scale[4], d[4];
        resolve(s, &base->offset, offset);
        resolve(s, &base->scale, scale);
        const float sign = e->subtract ? -1.f : 1.f;

        for (unsigned i = first; i < last; ++i) {
            out->offset[i] = offset[i];

            if (!is_constant(base))
                (base->texture == 1 ? out->w1 : out->w0)[i] = scale[i];
        }

        resolve(s, &light->offset, offset);
        resolve(s, &light->scale, scale);
        resolve(s, &e->input[3].offset, d);

        for (unsigned i = first; i < last; ++i) {
            out->kc[i] = sign * offset[i];
            out->kv[i] = is_constant(light) ? 0.f : sign * scale[i];
            out->d[i] = d[i] + equation_bias(e->bias);
            out->s[i] = equation_scale(e->scale);
        }
    }
}

static void layers_fill(const mh3_shade_material *m, const mh3_native_material_state *s, mh3_shade_layers *out) {
    memset(out, 0, sizeof *out);

    for (int alpha = 0; alpha < 2; ++alpha) {
        const mh3_shade_equation *e = alpha ? &m->alpha : &m->color;
        const mh3_shade_term *factor = &m->factor[alpha];
        const unsigned first = alpha ? 3 : 0, last = alpha ? 4 : 3;
        float offset[4], scale[4];

        for (unsigned r = 0; r < 4; ++r) {
            const mh3_shade_term *t = &e->input[r];
            resolve(s, &t->offset, offset);
            resolve(s, &t->scale, scale);

            for (unsigned i = first; i < last; ++i) {
                out->kc[r][i] = offset[i];
                out->kv[r][i] = t->source == MH3_SHADE_SOURCE_VERTEX ? scale[i] : 0.f;
            }
        }

        resolve(s, &factor->offset, offset);
        resolve(s, &factor->scale, scale);

        for (unsigned i = first; i < last; ++i) {
            out->s[i] = equation_scale(e->scale);
            out->mc[i] = offset[i];
            out->mv[i] = factor->source == MH3_SHADE_SOURCE_VERTEX ? scale[i] : 0.f;
        }
    }
}

void mh3_shade_fill(const mh3_shade_material *m, const mh3_native_material_state *s, mh3_shade_draw *draw) {
    if (m->model == MH3_SHADE_MODULATE)
        modulate_fill(m, s, &draw->fragment.shading.modulate);
    else if (m->model == MH3_SHADE_LAYERS)
        layers_fill(m, s, &draw->fragment.shading.layers);
    else {
        mh3_shade_basic *basic = &draw->fragment.shading.basic;

        for (unsigned i = 0; i < 4; ++i) {
            term(s, &m->color.input[i], &basic->color_input[i]);
            term(s, &m->alpha.input[i], &basic->alpha_input[i]);
        }

        equation(&m->color, basic->color_equation);
        equation(&m->alpha, basic->alpha_equation);
    }

    resolve(s, &m->overlay_scale, draw->fragment.overlay);
    alpha_range(s->pixel.values, draw->fragment.alpha_range);
}

/* ---- Material uniforms in GPU memory ---- */
/* One constant (two factors) as static * pick: entry is the palette entry of
 * its animated factor (MH3_SHADE_GPU_ONE when none); replicate/component
 * select one component of that entry for every lane. */
typedef struct {
    float s[4];
    uint8_t entry, replicate, component;
} gpu_split;

static int gpu_split_constant(const mh3_shade_constant *c, gpu_split *out) {
    for (unsigned i = 0; i < 4; ++i)
        out->s[i] = 1.f;

    out->entry = MH3_SHADE_GPU_ONE;
    out->replicate = 0;
    out->component = 3;

    for (unsigned f = 0; f < 2; ++f) {
        const uint8_t kind = c->kind[f], index = c->index[f], alpha = c->alpha[f];
        float value = 1.f;
        int entry = -1;
        uint8_t replicate = alpha ? 1 : 0, component = 3;

        switch (kind) {
        case MH3_SHADE_CONSTANT_ZERO:
            value = 0.f;
            break;
        case MH3_SHADE_CONSTANT_ONE:
            break;
        case MH3_SHADE_CONSTANT_HALF:
            value = 0.5f;
            break;
        case MH3_SHADE_CONSTANT_REGISTER:
            entry = index & 3;
            break;
        case MH3_SHADE_CONSTANT_KONST: {
            const int alpha_selector = (index & 0x80) != 0;
            const uint8_t selector = index & 0x7f;

            if (selector < 8)
                value = selector ? (8 - selector) / 8.f : 1.f;
            else if (!alpha_selector && selector >= 12 && selector < 16)
                entry = 4 + (selector - 12);
            else if (selector >= 16 && selector < 32) {
                entry = 4 + ((selector - 16) & 3);
                replicate = 1;
                component = (uint8_t)((selector - 16) / 4);
            } else
                value = 0.f;

            break;
        }
        case MH3_SHADE_CONSTANT_REGISTER_COMPLEMENT:
        case MH3_SHADE_CONSTANT_KONST_COMPLEMENT:
            return 0;
        default:
            value = index / 8.f;
            break;
        }

        if (entry < 0) {
            for (unsigned i = 0; i < 4; ++i)
                out->s[i] *= value;

            continue;
        }

        if (out->entry != MH3_SHADE_GPU_ONE)
            return 0;

        out->entry = (uint8_t)entry;
        out->replicate = replicate;
        out->component = component;
    }

    return 1;
}

static void gpu_mask(float mask[4], unsigned component) {
    for (unsigned i = 0; i < 4; ++i)
        mask[i] = i == component ? 1.f : 0.f;
}

static void gpu_static_pick(mh3_shade_gpu_pick *pick) {
    pick->index[0] = pick->index[1] = (float)MH3_SHADE_GPU_ONE;
    pick->index[2] = pick->index[3] = 0.f;
    gpu_mask(pick->color_mask, 3);
    gpu_mask(pick->alpha_mask, 3);
}

/* Colour lanes (alpha 0) or the alpha lane (alpha 1) of coefficient k from c,
 * scaled by sign. */
static int gpu_lanes(mh3_shade_gpu_material *out, unsigned k, int alpha, const mh3_shade_constant *c, float sign, uint8_t *dynamic) {
    gpu_split split;

    if (!gpu_split_constant(c, &split))
        return 0;

    mh3_shade_gpu_pick *pick = &out->pick[k];

    if (alpha) {
        out->coefficient[k][3] = sign * split.s[3];
        pick->index[1] = split.entry;
        gpu_mask(pick->alpha_mask, split.replicate ? split.component : 3);
    } else {
        for (unsigned i = 0; i < 3; ++i)
            out->coefficient[k][i] = sign * split.s[i];

        pick->index[0] = split.entry;
        pick->index[2] = split.replicate ? 1.f : 0.f;
        gpu_mask(pick->color_mask, split.component);
    }

    if (split.entry != MH3_SHADE_GPU_ONE)
        *dynamic = 1;

    return 1;
}

static const mh3_shade_constant gpu_zero = {{MH3_SHADE_CONSTANT_ZERO, MH3_SHADE_CONSTANT_ONE}, {0, 0}, {0, 0}};
int mh3_shade_gpu_bake(const mh3_shade_material *m, mh3_shade_gpu_material *out, uint8_t *dynamic) {
    memset(out, 0, sizeof *out);
    *dynamic = 0;

    for (unsigned k = 0; k < MH3_SHADE_GPU_COEFFICIENTS; ++k)
        gpu_static_pick(&out->pick[k]);

    int ok = 1;

    /* LAYERS has no Material block form: its draws take the Shade uniforms. */
    if (m->model == MH3_SHADE_LAYERS)
        return 0;

    if (m->model == MH3_SHADE_MODULATE) {
        /* modulate_fill, with each resolved constant kept as static * pick. */
        for (int alpha = 0; alpha < 2; ++alpha) {
            const mh3_shade_equation *e = alpha ? &m->alpha : &m->color;
            const unsigned first = alpha ? 3 : 0, last = alpha ? 4 : 3;
            const int swapped = !(texture_side(m, &e->input[1], alpha) && vertex_side(&e->input[2], alpha));
            const mh3_shade_term *base = &e->input[swapped ? 2 : 1], *light = &e->input[swapped ? 1 : 2];
            const float sign = e->subtract ? -1.f : 1.f;
            ok &= gpu_lanes(out, MH3_SHADE_GPU_OFFSET, alpha, &base->offset, 1.f, dynamic);

            if (!is_constant(base))
                ok &= gpu_lanes(out, base->texture == 1 ? MH3_SHADE_GPU_W1 : MH3_SHADE_GPU_W0, alpha, &base->scale, 1.f, dynamic);

            ok &= gpu_lanes(out, MH3_SHADE_GPU_KC, alpha, &light->offset, sign, dynamic);
            ok &= gpu_lanes(out, MH3_SHADE_GPU_KV, alpha, is_constant(light) ? &gpu_zero : &light->scale, sign, dynamic);
            ok &= gpu_lanes(out, MH3_SHADE_GPU_D, alpha, &e->input[3].offset, 1.f, dynamic);

            for (unsigned i = first; i < last; ++i) {
                out->add[i] = equation_bias(e->bias);
                out->scale[i] = equation_scale(e->scale);
            }
        }

        ok &= gpu_lanes(out, MH3_SHADE_GPU_MODULATE_OVERLAY, 0, &m->overlay_scale, 1.f, dynamic);
        ok &= gpu_lanes(out, MH3_SHADE_GPU_MODULATE_OVERLAY, 1, &m->overlay_scale, 1.f, dynamic);
    } else {
        for (unsigned i = 0; i < 4; ++i)
            for (int alpha = 0; alpha < 2; ++alpha) {
                const mh3_shade_term *t = &(alpha ? &m->alpha : &m->color)->input[i];
                ok &= gpu_lanes(out, MH3_SHADE_GPU_BASIC_OFFSET + i, alpha, &t->offset, 1.f, dynamic);
                ok &= gpu_lanes(out, MH3_SHADE_GPU_BASIC_SCALE + i, alpha, &t->scale, 1.f, dynamic);
                float *select = out->select[alpha * 4 + i];

                if (t->source == MH3_SHADE_SOURCE_TEXTURE)
                    select[t->texture < 3 ? t->texture : 0] = 1.f;
                else if (t->source == MH3_SHADE_SOURCE_VERTEX)
                    select[3] = 1.f;

                if (!alpha)
                    out->swizzle[i] = t->replicate_alpha ? 1.f : 0.f;
            }
        equation(&m->color, out->equation[0]);
        equation(&m->alpha, out->equation[1]);
        ok &= gpu_lanes(out, MH3_SHADE_GPU_BASIC_OVERLAY, 0, &m->overlay_scale, 1.f, dynamic);
        ok &= gpu_lanes(out, MH3_SHADE_GPU_BASIC_OVERLAY, 1, &m->overlay_scale, 1.f, dynamic);
    }

    return ok;
}

void mh3_shade_gpu_palette(const mh3_native_material_state *s, mh3_shade_gpu_animated *out) {
    for (unsigned j = 0; j < 4; ++j)
        for (unsigned i = 0; i < 4; ++i) {
            out->palette[j][i] = mh3_unit_value(s->color[j].rgba[i]);
            out->palette[4 + j][i] = mh3_unit_value(s->konst[j].rgba[i]);
        }
    for (unsigned i = 0; i < 4; ++i)
        out->palette[MH3_SHADE_GPU_ONE][i] = 1.f;
}

void mh3_shade_gpu_alpha_range(const uint32_t pixel[MH3_PIXEL_FIELD_COUNT], float out[4]) {
    alpha_range(pixel, out);
}

void mh3_shade_gpu_srt_identity(mh3_shade_gpu_srt *out) {
    const mh3_shade_gpu_srt identity = {{1.f, 1.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 0.f}};
    *out = identity;
}

void mh3_shade_gpu_srt_from_slot(const mh3_pack_texture_srt *slot, mh3_shade_gpu_srt *out) {
    mh3_shade_gpu_srt_identity(out);

    if (!(slot->flags & 2)) {
        out->scale_rotation[0] = slot->scale[0];
        out->scale_rotation[1] = slot->scale[1];
    }

    if (!(slot->flags & 4)) {
        const float angle = slot->rotation_degrees * 0.01745329251994329577f;
        out->scale_rotation[2] = sinf(angle);
        out->scale_rotation[3] = cosf(angle);
    }

    if (!(slot->flags & 8)) {
        out->translation[0] = slot->translation[0];
        out->translation[1] = slot->translation[1];
    }
}

void mh3_shade_gpu_srt_rows(const mh3_shade_gpu_srt *srt, float row0[3], float row1[3]) {
    const float su = srt->scale_rotation[0], sv = srt->scale_rotation[1], sine = srt->scale_rotation[2], cosine = srt->scale_rotation[3];
    const float tu = srt->translation[0], tv = srt->translation[1];
    const float a = su * cosine, b = su * sine, c = sv * sine, d = sv * cosine;
    row0[0] = a;
    row0[1] = -b;
    row0[2] = (b - a * tu) - b * tv;
    row1[0] = c;
    row1[1] = d;
    row1[2] = (-d - c * tu) + d * tv + 1.f;
}

void mh3_shade_gpu_coefficient(const mh3_shade_gpu_material *m, const mh3_shade_gpu_animated *a, unsigned k, float out[4]) {
    const mh3_shade_gpu_pick *p = &m->pick[k];
    const float *c = a->palette[(unsigned)p->index[0]], *alpha = a->palette[(unsigned)p->index[1]];
    float replicated = 0.f, picked = 0.f;

    for (unsigned i = 0; i < 4; ++i) {
        replicated += c[i] * p->color_mask[i];
        picked += alpha[i] * p->alpha_mask[i];
    }

    for (unsigned i = 0; i < 3; ++i)
        out[i] = m->coefficient[k][i] * (p->index[2] != 0.f ? replicated : c[i]);

    out[3] = m->coefficient[k][3] * picked;
}

void mh3_shade_gpu_resolve(const mh3_shade_material *material, const mh3_shade_gpu_material *m, const mh3_shade_gpu_animated *a, mh3_shade_fragment *out) {
    memset(out, 0, sizeof *out);
    memcpy(out->texture_bias, a->texture_bias, sizeof out->texture_bias);
    memcpy(out->alpha_range, a->alpha_range, sizeof out->alpha_range);

    if (material->model == MH3_SHADE_MODULATE) {
        mh3_shade_modulate *q = &out->shading.modulate;
        mh3_shade_gpu_coefficient(m, a, MH3_SHADE_GPU_W0, q->w0);
        mh3_shade_gpu_coefficient(m, a, MH3_SHADE_GPU_W1, q->w1);
        mh3_shade_gpu_coefficient(m, a, MH3_SHADE_GPU_OFFSET, q->offset);
        mh3_shade_gpu_coefficient(m, a, MH3_SHADE_GPU_KV, q->kv);
        mh3_shade_gpu_coefficient(m, a, MH3_SHADE_GPU_KC, q->kc);
        mh3_shade_gpu_coefficient(m, a, MH3_SHADE_GPU_D, q->d);

        for (unsigned i = 0; i < 4; ++i) {
            q->d[i] += m->add[i];
            q->s[i] = m->scale[i];
        }

        mh3_shade_gpu_coefficient(m, a, MH3_SHADE_GPU_MODULATE_OVERLAY, out->overlay);
        return;
    }

    mh3_shade_basic *q = &out->shading.basic;

    for (unsigned i = 0; i < 4; ++i) {
        float offset[4], scale[4];
        mh3_shade_gpu_coefficient(m, a, MH3_SHADE_GPU_BASIC_OFFSET + i, offset);
        mh3_shade_gpu_coefficient(m, a, MH3_SHADE_GPU_BASIC_SCALE + i, scale);
        memcpy(q->color_input[i].offset, offset, sizeof offset);
        memcpy(q->color_input[i].scale, scale, sizeof scale);
        memcpy(q->alpha_input[i].offset, offset, sizeof offset);
        memcpy(q->alpha_input[i].scale, scale, sizeof scale);
        memcpy(q->color_input[i].select, m->select[i], sizeof q->color_input[i].select);
        memcpy(q->alpha_input[i].select, m->select[4 + i], sizeof q->alpha_input[i].select);
        q->color_input[i].swizzle[0] = m->swizzle[i];
    }

    memcpy(q->color_equation, m->equation[0], sizeof q->color_equation);
    memcpy(q->alpha_equation, m->equation[1], sizeof q->alpha_equation);
    mh3_shade_gpu_coefficient(m, a, MH3_SHADE_GPU_BASIC_OVERLAY, out->overlay);
}

void mh3_shade_fog(uint32_t type, const mh3_shader_fog *fog, mh3_shade_scene *scene) {
    const uint32_t kind = type & 7;

    if (!type) {
        /* off: a finite zero distance and a zero curve */
        memset(scene->fog_color, 0, sizeof scene->fog_color + sizeof scene->fog + sizeof scene->fog_curve + sizeof scene->fog_shape);
        scene->fog[1] = 1.f;
        return;
    }

    for (unsigned i = 0; i < 4; ++i)
        scene->fog_color[i] = fog->color[i];

    if (type & 8) {
        scene->fog[0] = fog->a;
        scene->fog[1] = scene->fog[3] = 0.f;
    } else {
        scene->fog[0] = fog->a * 16777216.f;
        scene->fog[1] = fog->b;
        scene->fog[3] = ldexpf(1.f, 24 - (int)fog->b_shift);
    }

    scene->fog[2] = fog->c;
    /* amount = curve.w * f + shape.x + shape.y * exp2(-8 g), g = curve.x + curve.y * f (squared if curve.z) */
    scene->fog_curve[0] = kind == 6 || kind == 7 ? 1.f : 0.f;
    scene->fog_curve[1] = kind == 6 || kind == 7 ? -1.f : 1.f;
    scene->fog_curve[2] = kind == 5 || kind == 7 ? 1.f : 0.f;
    scene->fog_curve[3] = kind >= 4 ? 0.f : 1.f;
    scene->fog_shape[0] = kind == 4 || kind == 5 ? 1.f : 0.f;
    scene->fog_shape[1] = kind == 4 || kind == 5 ? -1.f : kind >= 6 ? 1.f : 0.f;
    scene->fog_shape[2] = (type & 8) ? 1.f : 0.f;
    scene->fog_shape[3] = 0.f;
}

/* Keep equality, overlay dirtiness and hashing on this declared input schema.
 * The classifier reads neither TEV clamp nor the other indirect recipe bytes. */
#define STRUCTURE_BIT(n) (UINT64_C(1) << (n))
#define TEV_STRUCTURE_MASK (((STRUCTURE_BIT(7) - 1) | STRUCTURE_BIT(MH3_TEV_COLOR_OUTPUT)) | \
    ((STRUCTURE_BIT(7) - 1) << MH3_TEV_ALPHA_A) | STRUCTURE_BIT(MH3_TEV_ALPHA_OUTPUT) | \
    (STRUCTURE_BIT(MH3_TEV_KONST_ALPHA + 1) - STRUCTURE_BIT(MH3_TEV_TEXTURE_MAP)) | STRUCTURE_BIT(MH3_TEV_INDIRECT_MATRIX))
#define PIXEL_STRUCTURE_MASK (STRUCTURE_BIT(MH3_PIXEL_ALPHA_COMPARE0) | STRUCTURE_BIT(MH3_PIXEL_ALPHA_COMPARE1) | STRUCTURE_BIT(MH3_PIXEL_ALPHA_OPERATION))
_Static_assert(sizeof(mh3_pack_tev_pass) == 9, "TEV field layout");
_Static_assert(offsetof(mh3_pack_tev_stage, indirect_add_previous) - offsetof(mh3_pack_tev_stage, color) == 33, "TEV field order");
static unsigned structure_stages(uint32_t count) {
    return count ? (count < 16 ? count : 16) : 1;
}

static uint32_t structure_tev_word(const mh3_pack_tev_stage *t, unsigned word) {
    const mh3_pack_tev_pass *p;

    if (word < 4) {
        p = word < 2 ? &t->color : &t->alpha;
        return !(word & 1) ? p->a | p->b << 8 | p->c << 16 | (uint32_t)p->d << 24 :
            p->operation | p->bias << 8 | p->scale << 16 | (uint32_t)p->output << 24;
    }

    if (word == 4)
        return t->texture_map | t->texture_coordinate << 8 | t->raster_channel << 16 | (uint32_t)t->konst_color << 24;

    return t->konst_alpha | t->texture_swap << 8 | t->raster_swap << 16 | (uint32_t)t->indirect_matrix << 24;
}

static uint32_t structure_swap_word(const mh3_pack_swap_table *swap) {
    return swap->rgba[0] | swap->rgba[1] << 8 | swap->rgba[2] << 16 | (uint32_t)swap->rgba[3] << 24;
}

int mh3_shade_structure_equal(const mh3_native_material_state *a, const mh3_native_material_state *b) {
    assert(a && b);

    if (a == b)
        return 1;

    if (a->tev_count != b->tev_count)
        return 0;

    for (unsigned i = 0; i < structure_stages(a->tev_count); ++i)
        for (unsigned word = 0; word < 6; ++word)
            if (structure_tev_word(&a->tev[i], word) != structure_tev_word(&b->tev[i], word))
                return 0;
    for (unsigned i = 0; i < 4; ++i) {
        const int complete = (a->swaps[i].known & 15) == 15;

        if (complete != ((b->swaps[i].known & 15) == 15) ||
            (complete && structure_swap_word(&a->swaps[i]) != structure_swap_word(&b->swaps[i])))
            return 0;
    }

    return a->pixel.values[MH3_PIXEL_ALPHA_COMPARE0] == b->pixel.values[MH3_PIXEL_ALPHA_COMPARE0] &&
           a->pixel.values[MH3_PIXEL_ALPHA_COMPARE1] == b->pixel.values[MH3_PIXEL_ALPHA_COMPARE1] &&
           a->pixel.values[MH3_PIXEL_ALPHA_OPERATION] == b->pixel.values[MH3_PIXEL_ALPHA_OPERATION];
}

int mh3_shade_structure_overlay_changes(const mh3_native_material_state *current, const mh3_native_material_state *source,
                                       uint32_t top_known, uint32_t groups) {
    assert(current && source && !(groups & ~MH3_SHADE_STRUCTURE_ALL));

    if (current == source)
        return 0;

    if ((top_known & MH3_MATERIAL_TOP_TEV_COUNT) && current->tev_count != source->tev_count)
        return 1;

    if (groups & MH3_SHADE_STRUCTURE_TEV)
        for (unsigned i = 0; i < structure_stages(current->tev_count); ++i) {
            uint64_t pending = source->tev[i].known & TEV_STRUCTURE_MASK;
            const uint8_t *to = (const uint8_t *)&current->tev[i].color;
            const uint8_t *from = (const uint8_t *)&source->tev[i].color;

            while (pending) {
                const unsigned field = (unsigned)__builtin_ctzll(pending);

                if (to[field] != from[field])
                    return 1;

                pending &= pending - 1;
            }
        }
    if (groups & MH3_SHADE_STRUCTURE_SWAPS)
        for (unsigned i = 0; i < 4; ++i) {
            const uint32_t known = source->swaps[i].known & 15;
            const int complete = (current->swaps[i].known & 15) == 15;
            const int next_complete = ((current->swaps[i].known | known) & 15) == 15;

            if (complete != next_complete)
                return 1;

            if (complete)
                for (unsigned field = 0; field < 4; ++field)
                    if ((known & (1u << field)) && current->swaps[i].rgba[field] != source->swaps[i].rgba[field])
                        return 1;
        }
    if (groups & MH3_SHADE_STRUCTURE_PIXEL) {
        uint64_t pending = source->pixel.known & PIXEL_STRUCTURE_MASK;

        while (pending) {
            const unsigned field = (unsigned)__builtin_ctzll(pending);

            if (current->pixel.values[field] != source->pixel.values[field])
                return 1;

            pending &= pending - 1;
        }
    }

    return 0;
}

uint64_t mh3_shade_structure_key(const mh3_native_material_state *s) {
    /* FNV-1a over the fields classification reads. */
    uint64_t h = 1469598103934665603ull;
#define MIX(value)                                                                                                                                                                                     \
    do {                                                                                                                                                                                               \
        h ^= (uint64_t)(value);                                                                                                                                                                        \
        h *= 1099511628211ull;                                                                                                                                                                         \
    } while (0)
    MIX(s->tev_count);

    for (unsigned i = 0; i < structure_stages(s->tev_count); ++i) {
        for (unsigned word = 0; word < 6; ++word)
            MIX(structure_tev_word(&s->tev[i], word));
    }

    for (unsigned i = 0; i < 4; ++i) {
        const int complete = (s->swaps[i].known & 15) == 15;
        MIX(complete);

        if (complete)
            MIX(structure_swap_word(&s->swaps[i]));
    }

    MIX(s->pixel.values[MH3_PIXEL_ALPHA_COMPARE0] | s->pixel.values[MH3_PIXEL_ALPHA_COMPARE1] << 8 | s->pixel.values[MH3_PIXEL_ALPHA_OPERATION] << 16);
#undef MIX
    return h;
}
#undef PIXEL_STRUCTURE_MASK
#undef TEV_STRUCTURE_MASK
#undef STRUCTURE_BIT
