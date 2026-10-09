#ifndef MH3_SHADE_H
#define MH3_SHADE_H
/* Fixed native shading models. A material is classified once into one of the
 * hand-written programs in runtime/src/native_shader_source.cpp plus a small
 * set of constant references; per draw only those references are resolved to
 * the current register/konst values. No TEV program is evaluated or generated.
 *
 * MODULATE (~95% of disc materials): one product per channel,
 *          out = ((t0 * w0 + t1 * w1 + offset) * (vertex * kv + kc) + d) * s,
 *          plus overlay * t1 * t2 in the base when OVERLAY is set.
 * BASIC:   out = (d + bias +- mix(a, b, c)) * scale, colour and alpha separately;
 *          each input is one texture role or the vertex colour times constants.
 * OVERLAY: overlay * texture1 * texture2 added to colour input b (the
 *          four-stage "base + masked overlay, then lit" material family).
 * LAYERS:  out = saturate(saturate((sum_r (vertex * kv_r + kc_r) * t_r +
 *          vertex * kv_3 + kc_3) * s) * (vertex * mv + mc)), colour and alpha
 *          separately: stages that add a texture times the vertex colour or a
 *          constant each (the Deserted Island sea surface, A-p09-uv-suimen*),
 *          then at most one multiply by the vertex colour or a constant.
 * Multi-stage programs are folded into this form when exact: prev * x
 * multiplies, prev + X layers (up to three) and stages that leave prev alone. */
#include "mh3_model_state.h"
#include "mh3_material_state.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- std140 uniform blocks, byte-identical to the shader declarations ---- */
/* offset + scale * (select . (texture0, texture1, texture2, vertex)), the
 * source's alpha replicated into colour when swizzle.x is set. */
typedef struct mh3_shade_input {
    float offset[4], scale[4], select[4], swizzle[4];
} mh3_shade_input;

typedef struct mh3_shade_texgen {
    float main[12], post[12];
    int32_t mode[4];
} mh3_shade_texgen;

enum {
    MH3_SHADE_TEXGEN_POSITION = 0,
    MH3_SHADE_TEXGEN_NORMAL = 1,
    MH3_SHADE_TEXGEN_COORDINATE = 2,
    MH3_SHADE_TEXGEN_PROJECTIVE = 1,
    MH3_SHADE_TEXGEN_NORMALIZE = 2,
    MH3_SHADE_TEXGEN_MAIN_IDENTITY = 4,
    MH3_SHADE_TEXGEN_POST_IDENTITY = 8,
    MH3_SHADE_TEXGEN_INDEXED = 16,
    MH3_SHADE_TEXGEN_POST_SRT = 32, /* material GPU: post from the role's Animated SRT */
    MH3_SHADE_TEXGEN_SKIN_ENVIRONMENT = 64, /* skinned INDEXED: the vertex's environment matrix, not its position matrix */
    MH3_SHADE_FLAG_MATRIX_SELECTOR = 1,
    MH3_SHADE_FLAG_COLOR0 = 2,
    MH3_SHADE_CHANNEL_COLOR_LIGHTING = 1,
    MH3_SHADE_CHANNEL_ALPHA_LIGHTING = 2,
    MH3_SHADE_CHANNEL_COLOR_MATERIAL_VERTEX = 4,
    MH3_SHADE_CHANNEL_COLOR_AMBIENT_VERTEX = 8,
    MH3_SHADE_CHANNEL_ALPHA_MATERIAL_VERTEX = 16,
    MH3_SHADE_CHANNEL_ALPHA_AMBIENT_VERTEX = 32
};

typedef struct mh3_shade_basic {
    mh3_shade_input color_input[4], alpha_input[4];
    float color_equation[4], alpha_equation[4]; /* bias, scale, sign */
} mh3_shade_basic;

typedef struct mh3_shade_modulate {
    float w0[4], w1[4], offset[4], kv[4], kc[4], d[4], s[4];
} mh3_shade_modulate;

/* LAYERS: kv/kc[r] scale texture role r (0..2), [3] is the textureless term. */
typedef struct mh3_shade_layers {
    float kv[4][4], kc[4][4], s[4], mv[4], mc[4];
} mh3_shade_layers;

/* The View block (mesh vertex stage, binding 6 / vertex BUFFER[4]): the
 * projection and viewport, shared by the draws that use them; a new block
 * only when they change. */
typedef struct mh3_shade_view {
    float proj[16];
    float viewport[4]; /* xy render size, zw clip-space pixel-centre correction */
} mh3_shade_view;

/* One per-draw allocation holds two std140 blocks: Draw (vertex stage,
 * binding 0 / vertex BUFFER[0]) at offset 0 and Shade (fragment stage,
 * binding 3 / fragment BUFFER[0]) at MH3_SHADE_FRAGMENT_OFFSET, so vertex
 * programs do not vary with the fragment model. Shade's tail is declared per
 * model; only MH3_SHADE_FRAGMENT_BYTES(model) bytes are written and bound.
 * A mesh draw writes Draw up to its last texture role's texgen
 * (MH3_SHADE_VERTEX_BYTES): its program reads no other role. */
typedef struct mh3_shade_vertex {
    int32_t flags[4];
    float channel_material[4], channel_ambient[4];
    int32_t channel[4]; /* x colour light mask, y alpha light mask, z bits, w functions */
    mh3_shade_texgen texgen[3];
} mh3_shade_vertex;

typedef struct mh3_shade_fragment {
    float texture_bias[4];
    float overlay[4];
    float alpha_range[4]; /* discard when alpha < x or alpha >= y */
    union {
        mh3_shade_basic basic;
        mh3_shade_modulate modulate;
        mh3_shade_layers layers;
    } shading;
} mh3_shade_fragment;

typedef struct mh3_shade_draw {
    mh3_shade_vertex vertex;
    float reserved[28]; /* Shade at MH3_SHADE_FRAGMENT_OFFSET */
    mh3_shade_fragment fragment;
} mh3_shade_draw;

enum {
    MH3_SHADE_BASIC,
    MH3_SHADE_MODULATE,
    MH3_SHADE_LAYERS
};
#define MH3_SHADE_FRAGMENT_OFFSET 512u
#define MH3_SHADE_VERTEX_BYTES(textures) ((uint32_t)offsetof(mh3_shade_vertex, texgen) + (uint32_t)(textures) * (uint32_t)sizeof(mh3_shade_texgen))
#define MH3_SHADE_FRAGMENT_BYTES(model) \
    (48u + ((model) == MH3_SHADE_MODULATE ? (uint32_t)sizeof(mh3_shade_modulate) : (model) == MH3_SHADE_LAYERS ? (uint32_t)sizeof(mh3_shade_layers) : (uint32_t)sizeof(mh3_shade_basic)))
#define MH3_SHADE_DRAW_BYTES(model) (MH3_SHADE_FRAGMENT_OFFSET + MH3_SHADE_FRAGMENT_BYTES(model))
/* Per vertex ze = fog.x / (fog.y - depth * fog.w) (perspective; fog.x = a * 2^24,
 * fog.w = 2^(24 - shift)) or fog.x * depth (shape.z set: orthographic); the
 * varying is ze - fog.z. Fragment amount = curve.w * f + shape.x + shape.y *
 * exp2(-8 g), g = curve.x + curve.y * f squared when curve.z is set. */
typedef struct mh3_shade_scene {
    mh3_shader_light lights[8];
    float fog_color[4], fog[4], fog_curve[4], fog_shape[4];
} mh3_shade_scene;

typedef struct mh3_shade_palette {
    float position[20][12], normal[10][12];
} mh3_shade_palette;

/* ---- Classification ---- */
enum {
    MH3_SHADE_CONSTANT_ZERO,
    MH3_SHADE_CONSTANT_ONE,
    MH3_SHADE_CONSTANT_HALF,
    MH3_SHADE_CONSTANT_REGISTER, /* index 0 prev, 1..3 reg0..2 */
    MH3_SHADE_CONSTANT_KONST,    /* index: GX konst colour or alpha selector */
    MH3_SHADE_CONSTANT_FRACTION  /* index n: n/8 (16 = 2) */
};

/* value = first * second; each factor optionally replicates its alpha. */
typedef struct mh3_shade_constant {
    uint8_t kind[2], index[2], alpha[2];
} mh3_shade_constant;

enum {
    MH3_SHADE_SOURCE_NONE,
    MH3_SHADE_SOURCE_TEXTURE,
    MH3_SHADE_SOURCE_VERTEX
};

/* input = offset + scale * source (source alpha replicated for colour if set);
 * texture is the material's texture role for a TEXTURE source. */
typedef struct mh3_shade_term {
    uint8_t source, texture, replicate_alpha;
    mh3_shade_constant offset, scale;
} mh3_shade_term;

typedef struct mh3_shade_equation {
    mh3_shade_term input[4];       /* a, b, c, d */
    uint8_t subtract, bias, scale; /* GX encodings: bias 0/+half/-half, scale 1/2/4/half */
} mh3_shade_equation;

enum {
    MH3_SHADE_EXACT = 1,     /* classification reproduces the authored equation */
    MH3_SHADE_SWAP = 2,      /* approximation: non-identity swap table ignored */
    MH3_SHADE_FOLDED = 4,    /* exact multi-stage fold */
    MH3_SHADE_FALLBACK = 8,  /* approximation: unmatched program or stage-written register */
    MH3_SHADE_COMPARE = 16,  /* approximation: comparison operation treated as add */
    MH3_SHADE_INDIRECT = 32, /* approximation: indirect texturing ignored */
    MH3_SHADE_CHANNEL1 = 64  /* approximation: colour channel 1 shaded as channel 0 */
};

typedef struct mh3_shade_material {
    uint8_t textures, overlay, alpha_test, quality, model;
    uint8_t texture_map[3], texture_coordinate[3];
    mh3_shade_equation color, alpha;
    mh3_shade_constant overlay_scale;
    /* LAYERS: input[r] is texture role r's factor (offset + scale * vertex),
     * input[3] the textureless term, scale the GX scale; factor[0/1] multiplies
     * the saturated colour/alpha (offset + scale * vertex). */
    mh3_shade_term factor[2];
    /* Cached exact form: saturate(texture0 * vertex), RGBA independently.
     * Only literal zero/one references qualify; draw-time register values do not. */
    uint8_t pure_modulate;
} mh3_shade_material;

/* Pure function of the material's structural fields; returns quality flags. */
uint8_t mh3_shade_classify(const mh3_native_material_state *state, mh3_shade_material *out);
/* Resolve the material's constant references against the current state and
 * write inputs, equations, overlay and alpha comparison into draw. */
void mh3_shade_fill(const mh3_shade_material *material, const mh3_native_material_state *state, mh3_shade_draw *draw);
/* Prepared GX fog (type: perspective 2..7, orthographic +8; 0 off) -> scene fog. */
void mh3_shade_fog(uint32_t type, const mh3_shader_fog *fog, mh3_shade_scene *scene);
/* Structural hash input for classification caches: equal keys classify equally. */
uint64_t mh3_shade_structure_key(const mh3_native_material_state *state);
/* Trusted classification input comparisons, with no hashing, candidate state,
 * padding or uniform comparisons. Restore compares complete effective inputs;
 * publication compares only the selected authored writes before mutation.
 * Swap values are consumed only with a complete four-component known mask.
 * Count0 uses stage0, matching the classifier's effective one-stage fallback. */
enum {
    MH3_SHADE_STRUCTURE_PIXEL = 1,
    MH3_SHADE_STRUCTURE_TEV = 2,
    MH3_SHADE_STRUCTURE_SWAPS = 4,
    MH3_SHADE_STRUCTURE_ALL = 7
};

int mh3_shade_structure_equal(const mh3_native_material_state *a, const mh3_native_material_state *b);
int mh3_shade_structure_overlay_changes(const mh3_native_material_state *current, const mh3_native_material_state *source,
                                       uint32_t top_known, uint32_t groups);

/* ---- Material uniforms in GPU memory (docs/optimizations/rendering/material-uniform-blocks.md) ----
 * A classified material splits into two std140 blocks:
 *  - Material (static): baked once per classification into persistent GPU
 *    memory. Every shading coefficient is a static factor times at most one
 *    animated palette entry (pick); the BASIC selects/equations and the
 *    MODULATE scale/bias are static.
 *  - Animated: the raw values that change at run time: TEV colour registers
 *    and konst colours, the alpha compare range, the texture LOD bias and
 *    each texture role's post SRT. Written per draw (or kept as a default).
 * The fragment program evaluates coefficient = static * pick(animated) from
 * uniforms only (GXM's secondary program); the mesh vertex program builds the
 * post texture matrix of an SRT role from scale, sin/cos and translation. */
enum {
    MH3_SHADE_GPU_PALETTE = 9, /* 0..3 TEV colour registers (prev, reg0..2), 4..7 konst, 8 one */
    MH3_SHADE_GPU_ONE = 8,
    MH3_SHADE_GPU_COEFFICIENTS = 9
};

/* Coefficient slots. MODULATE: w0, w1, offset, kv, kc, d, overlay.
 * BASIC: input offsets a..d (0..3), input scales a..d (4..7), overlay (8). */
enum {
    MH3_SHADE_GPU_W0,
    MH3_SHADE_GPU_W1,
    MH3_SHADE_GPU_OFFSET,
    MH3_SHADE_GPU_KV,
    MH3_SHADE_GPU_KC,
    MH3_SHADE_GPU_D,
    MH3_SHADE_GPU_MODULATE_OVERLAY,
    MH3_SHADE_GPU_BASIC_OFFSET = 0,
    MH3_SHADE_GPU_BASIC_SCALE = 4,
    MH3_SHADE_GPU_BASIC_OVERLAY = 8
};

/* Colour lanes read palette[index.x] (rgb, or the color_mask component in all
 * three lanes when index.z is 1); the alpha lane reads the alpha_mask
 * component of palette[index.y]. A static coefficient picks entry 8 (one). */
typedef struct mh3_shade_gpu_pick {
    float index[4];
    float color_mask[4], alpha_mask[4];
} mh3_shade_gpu_pick;

typedef struct mh3_shade_gpu_material {
    float coefficient[MH3_SHADE_GPU_COEFFICIENTS][4];
    mh3_shade_gpu_pick pick[MH3_SHADE_GPU_COEFFICIENTS];
    float add[4];         /* MODULATE: added to d (TEV bias) */
    float scale[4];       /* MODULATE: s */
    float equation[2][4]; /* BASIC colour, alpha: bias, scale, sign */
    float select[8][4];   /* BASIC: colour inputs a..d, then alpha inputs a..d */
    float swizzle[4];     /* BASIC: colour input i replicates its source's alpha */
} mh3_shade_gpu_material;

/* Post texture matrix of one texture role, built by the vertex program:
 *   a = su cos, b = su sin, c = sv sin, d = sv cos
 *   s' = a s - b t + (b - a tu - b tv), t' = c s + d t + (-d - c tu + d tv + 1)
 * which is the XSI SRT matrix of 80075F00 (every flag case). Identity:
 * (1, 1, 0, 1), (0, 0). */
typedef struct mh3_shade_gpu_srt {
    float scale_rotation[4]; /* su, sv, sin, cos */
    float translation[4];    /* tu, tv, 0, 0 */
} mh3_shade_gpu_srt;

typedef struct mh3_shade_gpu_animated {
    float palette[MH3_SHADE_GPU_PALETTE][4]; /* register/konst RGBA / 255; [8] = 1 */
    float alpha_range[4];                    /* discard when alpha < x or alpha >= y */
    float texture_bias[4];                   /* role LOD bias (GLES sampling) */
    mh3_shade_gpu_srt srt[3];
} mh3_shade_gpu_animated;

/* Bake the classified material's structure. Returns 0 when a constant has
 * two animated factors (no shader form; the caller keeps resolving on the
 * CPU). *dynamic is set when a coefficient picks an animated entry. */
int mh3_shade_gpu_bake(const mh3_shade_material *material, mh3_shade_gpu_material *out, uint8_t *dynamic);
/* byte / 255.f for every byte, folded at compile time: the A9 has no
 * pipelined divide, and a multiply by 1/255 rounds 126 bytes differently. */
extern const float mh3_unit_byte[256];

/* value / 255.f of a TEV colour register (signed 11-bit): the table for
 * 0-255, the divide otherwise. */
static inline float mh3_unit_value(int value) {
    return (unsigned)value < 256u ? mh3_unit_byte[value] : (float)value / 255.f;
}

/* The animated palette (TEV colour registers, konst colours, one). */
void mh3_shade_gpu_palette(const mh3_native_material_state *state, mh3_shade_gpu_animated *out);
/* GX alpha compare (pixel compare/reference/operation values) as the
 * discard interval [x, y) of the fragment programs. */
void mh3_shade_gpu_alpha_range(const uint32_t pixel[MH3_PIXEL_FIELD_COUNT], float out[4]);
void mh3_shade_gpu_srt_identity(mh3_shade_gpu_srt *out);
/* SRT of one live texture SRT slot (scale/rotation/translation already
 * defaulted by its identity flags); computes sin/cos of the rotation. */
void mh3_shade_gpu_srt_from_slot(const mh3_pack_texture_srt *slot, mh3_shade_gpu_srt *out);
/* Rows (3 floats each: s, t, constant) of the post matrix the vertex
 * program builds; reference for checks. */
void mh3_shade_gpu_srt_rows(const mh3_shade_gpu_srt *srt, float row0[3], float row1[3]);
/* Coefficient k of the fragment program: its static factor times its pick
 * of the animated palette. */
void mh3_shade_gpu_coefficient(const mh3_shade_gpu_material *baked, const mh3_shade_gpu_animated *animated, unsigned k, float out[4]);
/* Reference evaluation of the fragment program's uniform-only part: the
 * coefficients mh3_shade_fill would write, from the baked and animated
 * blocks. Writes overlay, alpha_range, texture_bias and the shading union. */
void mh3_shade_gpu_resolve(const mh3_shade_material *material, const mh3_shade_gpu_material *baked, const mh3_shade_gpu_animated *animated, mh3_shade_fragment *out);
#ifdef __cplusplus
}
#endif
#endif
