/* Synthetic records only; no ROM-derived bytes. */
#include "../../runtime/native/mh3_shade.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void near(float x, float y) { assert(fabsf(x - y) < 0.0001f); }
static mh3_pack_tev_pass pass(uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                              uint8_t scale, uint8_t output) {
  mh3_pack_tev_pass p = {a, b, c, d, 0, 0, scale, 1, output};
  return p;
}

static void stage(mh3_native_material_state *s, unsigned i,
                  mh3_pack_tev_pass color, mh3_pack_tev_pass alpha, uint8_t map,
                  uint8_t channel, uint8_t konst_color, uint8_t konst_alpha) {
  mh3_pack_tev_stage *t = &s->tev[i];
  memset(t, 0, sizeof *t);
  t->color = color;
  t->alpha = alpha;
  t->texture_map = map;
  t->texture_coordinate = map == 255 ? 255 : map;
  t->raster_channel = channel;
  t->konst_color = konst_color;
  t->konst_alpha = konst_alpha;

  if (i >= s->tev_count)
    s->tev_count = i + 1;
}

static void identity_swaps(mh3_native_material_state *s) {
  for (unsigned i = 0; i < 4; ++i) {
    s->swaps[i].known = 15;

    for (unsigned j = 0; j < 4; ++j)
      s->swaps[i].rgba[j] = (uint8_t)j;
  }
}

static void base(mh3_native_material_state *s) {
  memset(s, 0, sizeof *s);
  identity_swaps(s);
  s->pixel.values[MH3_PIXEL_ALPHA_COMPARE0] = 7;
  s->pixel.values[MH3_PIXEL_ALPHA_COMPARE1] = 7;
  s->pixel.values[MH3_PIXEL_ALPHA_OPERATION] = 0;
}

/* Evaluate an input the way the fragment shader does. */
static float input(const mh3_shade_input *in, unsigned k, const float tex[3][4],
                   const float ras[4]) {
  float source[4];

  for (unsigned i = 0; i < 4; ++i)
    source[i] = in->select[0] * tex[0][i] + in->select[1] * tex[1][i] +
                in->select[2] * tex[2][i] + in->select[3] * ras[i];
  return in->offset[k] +
         in->scale[k] * (in->swizzle[0] != 0.f ? source[3] : source[k]);
}

static float evaluate(const mh3_shade_material *m, const mh3_shade_draw *d,
                      int alpha, unsigned k, const float tex[3][4],
                      const float ras[4]) {
  if (m->model == MH3_SHADE_LAYERS) {
    const mh3_shade_layers *q = &d->fragment.shading.layers;
    float sum = ras[k] * q->kv[3][k] + q->kc[3][k];

    for (unsigned r = 0; r < 3; ++r)
      sum += (ras[k] * q->kv[r][k] + q->kc[r][k]) * tex[r][k];

    sum *= q->s[k];
    sum = sum < 0.f ? 0.f : sum > 1.f ? 1.f : sum;
    sum *= ras[k] * q->mv[k] + q->mc[k];
    return sum < 0.f ? 0.f : sum > 1.f ? 1.f : sum;
  }

  if (m->model == MH3_SHADE_MODULATE) {
    const mh3_shade_modulate *q = &d->fragment.shading.modulate;
    float base = tex[0][k] * q->w0[k] + tex[1][k] * q->w1[k] + q->offset[k];

    if (!alpha)
      base += d->fragment.overlay[k] * tex[1][k] * tex[2][k];

    return (base * (ras[k] * q->kv[k] + q->kc[k]) + q->d[k]) * q->s[k];
  }

  const mh3_shade_basic *q = &d->fragment.shading.basic;
  const mh3_shade_input *in = alpha ? q->alpha_input : q->color_input;
  const float *e = alpha ? q->alpha_equation : q->color_equation;
  float a = input(&in[0], k, tex, ras), b = input(&in[1], k, tex, ras),
        c = input(&in[2], k, tex, ras), dd = input(&in[3], k, tex, ras);
  if (!alpha)
    b += d->fragment.overlay[k] * tex[1][k] * tex[2][k];

  const float out = (dd + e[0] + e[2] * (a + (b - a) * c)) * e[1];

  if (!m->raster_product)
    return out;

  return (out < 0.f ? 0.f : out > 1.f ? 1.f : out) * ras[k];
}

/* Material uniforms in GPU memory: the baked block and the animated palette
 * resolve to the same fragment uniforms the programs would see. */
static void check_gpu(const mh3_shade_material *m,
                      const mh3_native_material_state *s, int alpha,
                      unsigned k, const float tex[3][4], const float ras[4],
                      float expect) {
  mh3_shade_gpu_material baked;
  mh3_shade_gpu_animated animated;
  uint8_t dynamic = 0;
  assert(mh3_shade_gpu_bake(m, &baked, &dynamic));
  memset(&animated, 0, sizeof animated);
  mh3_shade_gpu_palette(s, &animated);
  mh3_shade_gpu_alpha_range(s->pixel.values, animated.alpha_range);
  mh3_shade_draw d;
  memset(&d, 0, sizeof d);
  mh3_shade_gpu_resolve(m, &baked, &animated, &d.fragment);
  near(evaluate(m, &d, alpha, k, tex, ras), expect);
}

/* The classified model and the general BASIC program must both match. */
static void check(const mh3_shade_material *m,
                  const mh3_native_material_state *s, int alpha, unsigned k,
                  const float tex[3][4], const float ras[4], float expect) {
  mh3_shade_draw d;
  memset(&d, 0, sizeof d);
  mh3_shade_fill(m, s, &d);
  near(evaluate(m, &d, alpha, k, tex, ras), expect);
  check_gpu(m, s, alpha, k, tex, ras, expect);
  mh3_shade_material basic = *m;
  basic.model = MH3_SHADE_BASIC;
  memset(&d, 0, sizeof d);
  mh3_shade_fill(&basic, s, &d);
  near(evaluate(&basic, &d, alpha, k, tex, ras), expect);
  check_gpu(&basic, s, alpha, k, tex, ras, expect);
}

static void basic_texture_times_raster(void) {
  mh3_native_material_state s;
  base(&s);
  /* colour = texc * rasc ; alpha = texa * rasa */
  stage(&s, 0, pass(15, 8, 10, 15, 0, 0), pass(7, 4, 5, 7, 0, 0), 0, 0, 12, 0);
  mh3_shade_material m;
  const uint8_t q = mh3_shade_classify(&s, &m);
  assert(q == MH3_SHADE_EXACT);
  assert(m.textures == 1 && !m.overlay && !m.alpha_test &&
         m.model == MH3_SHADE_MODULATE);
  mh3_shade_draw d;
  memset(&d, 0, sizeof d);
  mh3_shade_fill(&m, &s, &d);
  const float tex[3][4] = {{0.5f, 0.25f, 1.f, 0.75f}},
              ras[4] = {0.5f, 1.f, 0.2f, 0.5f};
  for (unsigned k = 0; k < 3; ++k)
    check(&m, &s, 0, k, tex, ras, tex[0][k] * ras[k]);

  check(&m, &s, 1, 3, tex, ras, tex[0][3] * ras[3]);
}

static void konst_and_register(void) {
  mh3_native_material_state s;
  base(&s);
  /* colour = (c1 + mix(zero, texc, konst K1)) * 2 ; alpha = a0 */
  stage(&s, 0, pass(15, 8, 14, 4, 1, 0), pass(7, 7, 7, 1, 0, 0), 0, 0, 13, 0);
  s.konst[1].rgba[0] = 255;
  s.konst[1].rgba[1] = 51;
  s.konst[1].rgba[2] = 0;
  s.color[2].rgba[0] = 10;
  s.color[2].rgba[1] = 20;
  s.color[2].rgba[2] = 30;
  s.color[1].rgba[3] = 128;
  mh3_shade_material m;
  assert(mh3_shade_classify(&s, &m) == MH3_SHADE_EXACT);
  mh3_shade_draw d;
  mh3_shade_fill(&m, &s, &d);
  const float tex[3][4] = {{0.5f, 0.5f, 0.5f, 1.f}},
              ras[4] = {1.f, 1.f, 1.f, 1.f};
  const float expect[3] = {(10 / 255.f + 0.5f) * 2,
                           (20 / 255.f + 0.5f * 0.2f) * 2, (30 / 255.f) * 2};
  for (unsigned k = 0; k < 3; ++k)
    check(&m, &s, 0, k, tex, ras, expect[k]);

  check(&m, &s, 1, 3, tex, ras, 128 / 255.f);
}

static void folded_multiply(void) {
  mh3_native_material_state s;
  base(&s);
  /* s0: prev = texc * konst(7/8) ; s1: prev = prev * rasc */
  stage(&s, 0, pass(15, 8, 14, 15, 0, 0), pass(7, 4, 6, 7, 0, 0), 0, 0, 1, 1);
  stage(&s, 1, pass(15, 0, 10, 15, 0, 0), pass(7, 0, 5, 7, 0, 0), 255, 0, 0, 0);
  mh3_shade_material m;
  const uint8_t q = mh3_shade_classify(&s, &m);
  assert(q == (MH3_SHADE_EXACT | MH3_SHADE_FOLDED));
  mh3_shade_draw d;
  mh3_shade_fill(&m, &s, &d);
  const float tex[3][4] = {{0.4f, 0.6f, 0.8f, 0.5f}},
              ras[4] = {0.5f, 0.25f, 1.f, 0.5f};
  for (unsigned k = 0; k < 3; ++k)
    check(&m, &s, 0, k, tex, ras, tex[0][k] * 0.875f * ras[k]);

  check(&m, &s, 1, 3, tex, ras, tex[0][3] * 0.875f * ras[3]);
}

static void overlay_family(void) {
  mh3_native_material_state s;
  base(&s);
  /* s0: reg2 = tex0 * K0 ; s1: prev = tex1 * K1 + reg2 ; s2: prev = mix(reg2,
   * prev, tex2) ; s3: prev *= ras */
  stage(&s, 0, pass(15, 8, 14, 15, 0, 3), pass(7, 4, 6, 7, 0, 0), 0, 0, 12, 0);
  stage(&s, 1, pass(15, 8, 14, 6, 0, 0), pass(7, 7, 7, 0, 0, 0), 1, 0, 13, 0);
  stage(&s, 2, pass(6, 0, 8, 15, 0, 0), pass(7, 7, 7, 0, 0, 0), 2, 0, 0, 0);
  stage(&s, 3, pass(15, 0, 10, 15, 1, 0), pass(7, 0, 5, 7, 0, 0), 255, 0, 0, 0);
  s.konst[0].rgba[0] = s.konst[0].rgba[1] = s.konst[0].rgba[2] = 255;
  s.konst[1].rgba[0] = 255;
  s.konst[1].rgba[1] = 128;
  s.konst[1].rgba[2] = 0;
  mh3_shade_material m;
  const uint8_t q = mh3_shade_classify(&s, &m);
  assert(q & MH3_SHADE_EXACT);
  assert(m.overlay && m.textures == 3 && m.model == MH3_SHADE_MODULATE);
  mh3_shade_draw d;
  mh3_shade_fill(&m, &s, &d);
  const float tex[3][4] = {{0.2f, 0.4f, 0.6f, 1.f},
                           {1.f, 0.5f, 0.25f, 1.f},
                           {0.5f, 0.5f, 1.f, 1.f}},
              ras[4] = {0.5f, 0.5f, 0.5f, 1.f};
  const float k1[3] = {1.f, 128 / 255.f, 0.f};

  for (unsigned k = 0; k < 3; ++k) {
    const float reg2 = tex[0][k];
    const float prev = reg2 + tex[1][k] * k1[k] * tex[2][k];
    check(&m, &s, 0, k, tex, ras, prev * ras[k] * 2);
  }
  /* raster form: reg2 = t0 * ras ; prev = t1 * ras + reg2 ; mix(reg2, prev, t2)
   */
  base(&s);
  stage(&s, 0, pass(15, 8, 10, 15, 0, 3), pass(7, 4, 5, 7, 0, 0), 0, 0, 0, 0);
  stage(&s, 1, pass(15, 8, 10, 6, 0, 0), pass(7, 7, 7, 0, 0, 0), 1, 0, 0, 0);
  stage(&s, 2, pass(6, 0, 8, 15, 0, 0), pass(7, 7, 7, 0, 0, 0), 2, 6, 0, 0);
  assert(mh3_shade_classify(&s, &m) == MH3_SHADE_EXACT);
  assert(m.overlay);
  mh3_shade_fill(&m, &s, &d);

  for (unsigned k = 0; k < 3; ++k)
    check(&m, &s, 0, k, tex, ras, (tex[0][k] + tex[1][k] * tex[2][k]) * ras[k]);

  check(&m, &s, 1, 3, tex, ras, tex[0][3] * ras[3]);
}

/* em024 wings: the overlay family, then a last stage writing a0 = map3.a *
 * ras.a (GX outputs the last stage whatever its register). The overlay
 * holds all three roles, so it is dropped: colour keeps the base texture and
 * alpha samples map 3 instead of the zero left in prev. */
static void layers(void) {
  mh3_native_material_state s;
  base(&s);
  /* prev = t0*K0 ; prev += t1*K1 ; prev += t2*K2 (colour and alpha) */
  stage(&s, 0, pass(15, 8, 14, 15, 0, 0), pass(7, 4, 6, 7, 0, 0), 0, 6, 12, 28);
  stage(&s, 1, pass(15, 8, 14, 0, 0, 0), pass(7, 4, 6, 0, 0, 0), 1, 6, 13, 29);
  stage(&s, 2, pass(15, 8, 14, 0, 0, 0), pass(7, 4, 6, 0, 0, 0), 2, 6, 14, 30);

  for (unsigned i = 0; i < 3; ++i)
    for (unsigned j = 0; j < 4; ++j)
      s.konst[i].rgba[j] = (int16_t)(40 + 60 * i + 10 * j);
  mh3_shade_material m;
  assert(mh3_shade_classify(&s, &m) == (MH3_SHADE_EXACT | MH3_SHADE_FOLDED));
  assert(m.textures == 3 && !m.overlay && m.model == MH3_SHADE_BASIC);
  mh3_shade_draw d;
  mh3_shade_fill(&m, &s, &d);
  const float tex[3][4] = {{0.2f, 0.4f, 0.6f, 0.8f},
                           {0.9f, 0.1f, 0.5f, 0.3f},
                           {0.3f, 0.7f, 0.2f, 0.6f}},
              ras[4] = {1.f, 1.f, 1.f, 1.f};
  for (unsigned k = 0; k < 3; ++k) {
    float expect = 0.f;

    for (unsigned i = 0; i < 3; ++i)
      expect += tex[i][k] * s.konst[i].rgba[k] / 255.f;

    check(&m, &s, 0, k, tex, ras, expect);
  }

  float alpha = 0.f;

  for (unsigned i = 0; i < 3; ++i)
    alpha += tex[i][3] * s.konst[i].rgba[3] / 255.f;

  check(&m, &s, 1, 3, tex, ras, alpha);
  /* prev = t0*K0 ; prev += mix(t1, 0, K1) (inverse weight) */
  base(&s);
  stage(&s, 0, pass(15, 8, 14, 15, 0, 0), pass(7, 4, 6, 7, 0, 0), 0, 6, 12, 0);
  stage(&s, 1, pass(8, 15, 14, 0, 0, 0), pass(4, 7, 6, 0, 0, 0), 1, 6, 12, 0);
  s.konst[0].rgba[0] = 64;
  s.konst[0].rgba[1] = 128;
  s.konst[0].rgba[2] = 192;
  assert(mh3_shade_classify(&s, &m) == (MH3_SHADE_EXACT | MH3_SHADE_FOLDED));
  mh3_shade_fill(&m, &s, &d);

  for (unsigned k = 0; k < 3; ++k) {
    const float k0 = s.konst[0].rgba[k] / 255.f;
    check(&m, &s, 0, k, tex, ras, tex[0][k] * k0 + tex[1][k] * (1.f - k0));
  }
}

static void split_color_alpha(void) {
  mh3_native_material_state s;
  base(&s);
  /* colour from texture map 1, alpha from map 0: s0 c = 0, a = t0a*Ka ;
   * s1 c = t1*K ; s2 c = prev * ras, a = prev * rasa */
  stage(&s, 0, pass(15, 15, 15, 15, 0, 0), pass(7, 4, 6, 7, 0, 0), 0, 6, 12, 0);
  stage(&s, 1, pass(15, 8, 14, 15, 0, 0), pass(7, 7, 7, 0, 0, 0), 1, 6, 12, 0);
  stage(&s, 2, pass(15, 10, 0, 15, 0, 0), pass(7, 5, 0, 7, 0, 0), 255, 0, 0, 0);
  s.konst[0].rgba[0] = s.konst[0].rgba[1] = s.konst[0].rgba[2] = 255;
  mh3_shade_material m;
  assert(mh3_shade_classify(&s, &m) == (MH3_SHADE_EXACT | MH3_SHADE_FOLDED));
  assert(m.textures == 2 && m.model == MH3_SHADE_MODULATE);
  mh3_shade_draw d;
  mh3_shade_fill(&m, &s, &d);
  /* role order follows classification: colour's map first */
  assert(m.texture_map[0] == 1 && m.texture_map[1] == 0);
  const float tex[3][4] = {{0.5f, 0.6f, 0.7f, 0.25f}, {0.1f, 0.2f, 0.3f, 0.9f}},
              ras[4] = {0.5f, 0.25f, 0.75f, 0.5f};
  for (unsigned k = 0; k < 3; ++k)
    check(&m, &s, 0, k, tex, ras, tex[0][k] * ras[k]);

  check(&m, &s, 1, 3, tex, ras, tex[1][3] * ras[3]);
}

/* The Deserted Island sea surface (A-p09-uv-suimen1b; AL-p09-uv-suimen scales
 * its last colour stage by 2): colour = ras * t0 + ras * t2 + t1 / 2 + a1 * t0,
 * alpha = saturate(2 * t0 + a0) * ras, roles in stage order (maps 0, 2, 1). */
static void sea_surface(void) {
  for (uint8_t scale = 0; scale < 2; ++scale) {
    mh3_native_material_state s;
    base(&s);
    stage(&s, 0, pass(15, 10, 8, 15, 0, 0), pass(4, 7, 7, 4, 0, 0), 0, 4, 12, 28);
    stage(&s, 1, pass(15, 10, 8, 0, 0, 0), pass(1, 7, 7, 0, 0, 0), 2, 4, 27, 27);
    stage(&s, 2, pass(15, 13, 8, 0, 0, 0), pass(7, 7, 7, 0, 0, 0), 1, 4, 23, 23);
    stage(&s, 3, pass(15, 5, 8, 0, scale, 0), pass(7, 5, 0, 7, 0, 0), 0, 4, 12, 28);
    s.color[1].rgba[3] = 202;
    s.color[2].rgba[3] = 35;
    mh3_shade_material m;
    assert(mh3_shade_classify(&s, &m) == MH3_SHADE_EXACT);
    assert(m.model == MH3_SHADE_LAYERS && m.textures == 3 && !m.overlay);
    assert(m.texture_map[0] == 0 && m.texture_map[1] == 2 && m.texture_map[2] == 1);
    mh3_shade_gpu_material baked;
    uint8_t dynamic = 0;
    assert(!mh3_shade_gpu_bake(&m, &baked, &dynamic));
    mh3_shade_draw d;
    memset(&d, 0, sizeof d);
    mh3_shade_fill(&m, &s, &d);
    const float tex[3][4] = {{0.2f, 0.4f, 0.1f, 0.2f}, {0.3f, 0.1f, 0.4f, 1.f}, {0.5f, 0.6f, 0.7f, 1.f}},
                faint[3][4] = {{0.2f, 0.4f, 0.1f, 0.05f}},
                ras[4] = {0.3f, 0.5f, 0.6f, 0.8f};

    for (unsigned k = 0; k < 3; ++k) {
      float expect = (ras[k] * tex[0][k] + ras[k] * tex[1][k] + 0.5f * tex[2][k] + 35.f / 255.f * tex[0][k]) * (scale ? 2.f : 1.f);
      expect = expect > 1.f ? 1.f : expect;
      near(evaluate(&m, &d, 0, k, tex, ras), expect);
    }

    near(evaluate(&m, &d, 1, 3, tex, ras), 0.8f);
    near(evaluate(&m, &d, 1, 3, faint, ras), (0.1f + 202.f / 255.f) * 0.8f);
  }
}

/* The Tundra cave ice (B-p02-kabeE, B-p02-jimen): colour =
 * saturate(2 * ras * (c0 + t0 * Ka + mix(t1, 0, Ka))), alpha the same without
 * c0 and the 2: the inverse konst weight and the scaled last multiply. */
static float clamp01(float x) { return x < 0.f ? 0.f : x > 1.f ? 1.f : x; }

static void cave_ice(void) {
  mh3_native_material_state s;
  base(&s);
  stage(&s, 0, pass(15, 8, 14, 2, 0, 0), pass(7, 4, 6, 7, 0, 0), 0, 6, 31, 31);
  stage(&s, 1, pass(8, 15, 14, 0, 0, 0), pass(4, 7, 6, 0, 0, 0), 1, 6, 31, 31);
  stage(&s, 2, pass(15, 0, 10, 15, 1, 0), pass(7, 0, 5, 7, 0, 0), 255, 4, 12, 28);
  s.color[1].rgba[0] = 40;
  s.color[1].rgba[1] = 60;
  s.color[1].rgba[2] = 80;
  s.konst[3].rgba[3] = 170;
  mh3_shade_material m;
  assert(mh3_shade_classify(&s, &m) == MH3_SHADE_EXACT);
  assert(m.model == MH3_SHADE_LAYERS && m.textures == 2);
  mh3_shade_draw d;
  memset(&d, 0, sizeof d);
  mh3_shade_fill(&m, &s, &d);
  const float tex[3][4] = {{0.6f, 0.3f, 0.9f, 0.5f}, {0.2f, 0.8f, 0.4f, 0.7f}},
              ras[2][4] = {{0.1f, 0.2f, 0.15f, 0.6f}, {0.9f, 0.7f, 0.8f, 1.f}};
  const float ka = 170.f / 255.f;

  for (unsigned r = 0; r < 2; ++r) {
    for (unsigned k = 0; k < 3; ++k) {
      const float sum = clamp01(s.color[1].rgba[k] / 255.f + tex[0][k] * ka + tex[1][k] * (1.f - ka));
      near(evaluate(&m, &d, 0, k, tex, ras[r]), clamp01(2.f * ras[r][k] * sum));
    }

    near(evaluate(&m, &d, 1, 3, tex, ras[r]), clamp01(tex[0][3] * ka + tex[1][3] * (1.f - ka)) * ras[r][3]);
  }
}

/* The Loc Lac city walls: a baked shadow mask (texture 1) blends the lit
 * texture toward the shadow colour c0, then the vertex colour multiplies:
 * colour = ras * 2 * mix(c0, t0 * K3a, t1), alpha = (t0 K3a + t1 (1 - K3a)) * ras. */
static void city_wall(void) {
  mh3_native_material_state s;
  base(&s);
  stage(&s, 0, pass(15, 8, 14, 15, 0, 0), pass(7, 4, 6, 7, 0, 0), 0, 6, 31, 31);
  stage(&s, 1, pass(2, 0, 8, 15, 1, 0), pass(4, 7, 6, 0, 0, 0), 1, 6, 31, 31);
  stage(&s, 2, pass(15, 10, 0, 15, 0, 0), pass(7, 5, 0, 7, 0, 0), 255, 4, 12, 28);
  s.color[1].rgba[0] = 40;
  s.color[1].rgba[1] = 60;
  s.color[1].rgba[2] = 80;
  s.konst[3].rgba[3] = 170;
  mh3_shade_material m;
  assert(mh3_shade_classify(&s, &m) == (MH3_SHADE_EXACT | MH3_SHADE_FOLDED));
  assert(m.model == MH3_SHADE_BASIC && m.textures == 2 && m.raster_product == 3);
  const float tex[3][4] = {{0.6f, 0.3f, 0.9f, 0.5f}, {0.2f, 0.8f, 0.4f, 0.7f}},
              ras[2][4] = {{0.1f, 0.2f, 0.15f, 0.6f}, {0.9f, 0.7f, 0.8f, 1.f}};
  const float ka = 170.f / 255.f;

  for (unsigned r = 0; r < 2; ++r) {
    for (unsigned k = 0; k < 3; ++k) {
      const float c0 = s.color[1].rgba[k] / 255.f;
      check(&m, &s, 0, k, tex, ras[r], clamp01(2.f * (c0 + (tex[0][k] * ka - c0) * tex[1][k])) * ras[r][k]);
    }

    check(&m, &s, 1, 3, tex, ras[r], clamp01(tex[0][3] * ka + tex[1][3] * (1.f - ka)) * ras[r][3]);
  }
}

/* The CHAIN program's arithmetic (native_shader_source.cpp), lane k (3:
 * alpha), with the texture of each role's map from tex. */
static float chain_lane(const mh3_shade_material *m, const mh3_shade_draw *d,
                        unsigned k, const float tex[8][4], const float ras[4]) {
  const mh3_shade_chain_uniforms *u = &d->fragment.shading.chain;
  const mh3_shade_chain_form *f = &mh3_shade_chain_forms[m->chain];
  float t[5][4];

  for (unsigned r = 0; r < 5; ++r)
    for (unsigned i = 0; i < 4; ++i)
      t[r][i] = r < m->textures && m->texture_map[r] < 8
                    ? tex[m->texture_map[r]][i]
                    : 1.f;

  if (k == 3) {
    const float a = u->alpha_select[0] * t[0][3] + u->alpha_select[1] * t[1][3] +
                    u->alpha_select[2] * t[2][3] + u->alpha_select[3] * t[3][3] +
                    u->alpha_select2[0] * t[4][3] + u->alpha_select2[1] * ras[3] +
                    u->alpha_select2[2];
    return clamp01(a * (ras[3] * u->alpha_factor[0] + u->alpha_factor[1]));
  }

  float x = clamp01(t[0][k] * (ras[k] * u->base_v[k] + u->base_c[k]));

  for (unsigned i = 0; i < f->steps; ++i) {
    const mh3_shade_chain_step_uniforms *q = &u->step[i];
    const float layer = f->layer[i] != MH3_SHADE_NO_ROLE ? t[f->layer[i]][k] : 1.f;
    const float mask = f->mask[i] != MH3_SHADE_NO_ROLE ? t[f->mask[i]][k] : 1.f;
    const float p = clamp01((x * (ras[k] * q->keep_v[k] + q->keep_c[k]) +
                             layer * (ras[k] * q->layer_v[k] + q->layer_c[k])) *
                            q->s[k]);
    x += (p - x) * (mask * q->mask_scale[k] + q->mask_offset[k]);
  }

  return x;
}

/* Ceadeus's body (em020 b_asme_0): the lit base, a masked environment map,
 * then a masked rim map; five textures, the masks at the base's coordinate:
 *   c2 = t0 ras; prev = sat(c2 + t1 k1); c0 = mix(c2, prev, t2);
 *   prev = sat(c0 + t3 k0); prev = mix(c0, prev, t4); alpha t0.a ras.a. */
static void chain_masked_layers(void) {
  mh3_native_material_state s;
  base(&s);
  stage(&s, 0, pass(15, 8, 10, 15, 0, 3), pass(7, 4, 5, 7, 0, 0), 0, 4, 0, 0);
  stage(&s, 1, pass(15, 8, 14, 6, 0, 0), pass(7, 7, 7, 0, 0, 0), 1, 6, 13, 0);
  stage(&s, 2, pass(6, 0, 8, 15, 0, 1), pass(7, 7, 7, 0, 0, 0), 2, 6, 0, 0);
  stage(&s, 3, pass(15, 8, 14, 2, 0, 0), pass(7, 7, 7, 0, 0, 0), 3, 6, 12, 0);
  stage(&s, 4, pass(2, 0, 8, 15, 0, 0), pass(7, 0, 6, 7, 0, 0), 4, 6, 0, 0);
  const uint8_t k0[3] = {200, 180, 160}, k1[3] = {90, 120, 150};

  for (unsigned i = 0; i < 3; ++i) {
    s.konst[0].rgba[i] = k0[i];
    s.konst[1].rgba[i] = k1[i];
  }

  mh3_shade_material m;
  assert(mh3_shade_classify(&s, &m) == (MH3_SHADE_EXACT | MH3_SHADE_FOLDED));
  assert(m.model == MH3_SHADE_CHAIN && m.textures == 5);
  /* layers in roles 1 and 2, masks in roles 3 and 4 */
  assert(m.texture_map[0] == 0 && m.texture_map[1] == 1 && m.texture_map[2] == 3 &&
         m.texture_map[3] == 2 && m.texture_map[4] == 4);
  mh3_shade_draw d;
  memset(&d, 0, sizeof d);
  mh3_shade_fill(&m, &s, &d);
  const float tex[8][4] = {{0.6f, 0.3f, 0.9f, 0.5f}, {0.7f, 0.9f, 0.4f, 0.1f},
                           {0.5f, 0.2f, 0.8f, 0.3f}, {0.9f, 0.6f, 0.7f, 0.2f},
                           {0.25f, 1.f, 0.6f, 0.4f}},
              ras[2][4] = {{0.4f, 0.8f, 0.2f, 0.6f}, {1.f, 0.9f, 0.7f, 1.f}};

  for (unsigned r = 0; r < 2; ++r) {
    for (unsigned k = 0; k < 3; ++k) {
      const float c2 = tex[0][k] * ras[r][k];
      const float c0 = c2 + tex[2][k] * (clamp01(c2 + tex[1][k] * k1[k] / 255.f) - c2);
      const float glow = clamp01(c0 + tex[3][k] * k0[k] / 255.f);
      near(chain_lane(&m, &d, k, tex, ras[r]), c0 + tex[4][k] * (glow - c0));
    }

    near(chain_lane(&m, &d, 3, tex, ras[r]), tex[0][3] * ras[r][3]);
  }
}

/* Lagiacrus's spines (em015 bsm_2): the masked environment map, lit, then
 * a glow added after the lighting, in the konst colour k3:
 *   c2 = t0; prev = sat(c2 + t1 k1); prev = mix(c2, prev, t2);
 *   prev = prev ras; prev = sat(prev + t3 k3); alpha ras.a. */
static void chain_lit_glow(void) {
  mh3_native_material_state s;
  base(&s);
  stage(&s, 0, pass(15, 8, 14, 15, 0, 3), pass(7, 7, 7, 7, 0, 0), 0, 6, 0, 0);
  stage(&s, 1, pass(15, 8, 14, 6, 0, 0), pass(7, 7, 7, 7, 0, 0), 1, 6, 13, 0);
  stage(&s, 2, pass(6, 0, 8, 15, 0, 0), pass(7, 7, 7, 7, 0, 0), 2, 6, 0, 0);
  stage(&s, 3, pass(15, 0, 10, 15, 0, 0), pass(7, 6, 5, 7, 0, 0), 255, 4, 0, 0);
  stage(&s, 4, pass(15, 8, 14, 0, 0, 0), pass(7, 7, 7, 0, 0, 0), 3, 6, 15, 0);
  const uint8_t k1[3] = {255, 255, 255}, k3[3] = {100, 220, 255};

  for (unsigned i = 0; i < 3; ++i) {
    s.konst[1].rgba[i] = k1[i];
    s.konst[3].rgba[i] = k3[i];
  }

  mh3_shade_material m;
  assert(mh3_shade_classify(&s, &m) == (MH3_SHADE_EXACT | MH3_SHADE_FOLDED));
  assert(m.model == MH3_SHADE_CHAIN && m.textures == 4 && m.chain == 4);
  mh3_shade_draw d;
  memset(&d, 0, sizeof d);
  mh3_shade_fill(&m, &s, &d);
  const float tex[8][4] = {{0.6f, 0.3f, 0.9f, 0.5f}, {0.7f, 0.9f, 0.4f, 0.1f},
                           {0.5f, 0.2f, 0.8f, 0.3f}, {0.9f, 0.6f, 0.7f, 0.2f}},
              ras[4] = {0.4f, 0.8f, 0.2f, 0.6f};

  for (unsigned k = 0; k < 3; ++k) {
    const float c2 = tex[0][k];
    const float lit = (c2 + tex[2][k] * (clamp01(c2 + tex[1][k] * k1[k] / 255.f) - c2)) * ras[k];
    near(chain_lane(&m, &d, k, tex, ras), clamp01(lit + tex[3][k] * k3[k] / 255.f));
  }

  near(chain_lane(&m, &d, 3, tex, ras), ras[3]);
}

/* Alatreon's wings (em024 basm_2): the masked environment map at twice its
 * sum, and the alpha from a fourth map that the last stage writes to a0
 * (the TEV's output is the last stage's): textures 0, 1 and the mask 2, the
 * alpha map 3, which takes the free role 2 (its own coordinate). */
static void chain_alpha_map(void) {
  mh3_native_material_state s;
  base(&s);
  s.pixel.values[MH3_PIXEL_ALPHA_COMPARE0] = 6; /* GEQUAL 100 */
  s.pixel.values[MH3_PIXEL_ALPHA_REFERENCE0] = 100;
  s.pixel.values[MH3_PIXEL_ALPHA_COMPARE1] = 3; /* LEQUAL 255 */
  s.pixel.values[MH3_PIXEL_ALPHA_REFERENCE1] = 255;
  stage(&s, 0, pass(15, 8, 10, 15, 0, 3), pass(7, 7, 7, 7, 0, 0), 0, 0, 0, 0);
  stage(&s, 1, pass(15, 8, 14, 6, 1, 0), pass(7, 7, 7, 0, 0, 0), 1, 6, 15, 0);
  stage(&s, 2, pass(6, 0, 8, 15, 0, 0), pass(7, 7, 7, 0, 0, 0), 2, 6, 0, 0);
  stage(&s, 3, pass(14, 15, 12, 0, 0, 0), pass(7, 4, 5, 7, 0, 1), 3, 0, 0, 0);
  const uint8_t k3[3] = {255, 65, 30};

  for (unsigned i = 0; i < 3; ++i)
    s.konst[3].rgba[i] = k3[i];

  mh3_shade_material m;
  assert(mh3_shade_classify(&s, &m) == (MH3_SHADE_EXACT | MH3_SHADE_FOLDED));
  assert(m.model == MH3_SHADE_CHAIN && m.alpha_test && m.textures == 4 && m.chain == 1);
  assert(m.texture_map[2] == 3 && m.texture_coordinate[2] == 3 && m.texture_map[3] == 2);
  mh3_shade_draw d;
  memset(&d, 0, sizeof d);
  mh3_shade_fill(&m, &s, &d);
  const float tex[8][4] = {{0.2f, 0.4f, 0.6f, 0.f}, {0.3f, 0.1f, 0.5f, 0.2f},
                           {0.7f, 0.5f, 0.25f, 0.f}, {0.9f, 0.1f, 0.5f, 0.75f}},
              ras[4] = {0.5f, 0.5f, 0.5f, 0.8f};

  for (unsigned k = 0; k < 3; ++k) {
    const float c2 = tex[0][k] * ras[k];
    near(chain_lane(&m, &d, k, tex, ras), c2 + tex[2][k] * (clamp01(2.f * (c2 + tex[1][k] * k3[k] / 255.f)) - c2));
  }

  near(chain_lane(&m, &d, 3, tex, ras), 0.75f * 0.8f);
  near(d.fragment.alpha_range[0], 99.5f / 255.f);
}

static void approximations(void) {
  mh3_native_material_state s;
  base(&s);
  /* comparison op and a second texture in a non-overlay program */
  stage(&s, 0, pass(15, 8, 10, 15, 0, 0), pass(7, 4, 5, 7, 0, 0), 0, 0, 12, 0);
  stage(&s, 1, pass(0, 8, 10, 15, 0, 0), pass(7, 7, 7, 0, 0, 0), 1, 0, 0, 0);
  s.tev[1].color.operation = 8;
  s.pixel.values[MH3_PIXEL_ALPHA_COMPARE0] = 4;
  s.pixel.values[MH3_PIXEL_ALPHA_REFERENCE0] = 128;
  mh3_shade_material m;
  const uint8_t q = mh3_shade_classify(&s, &m);
  assert(!(q & MH3_SHADE_EXACT) && (q & MH3_SHADE_FALLBACK));
  assert(m.alpha_test);
  mh3_shade_draw d;
  mh3_shade_fill(&m, &s, &d);
  /* GREATER 128 AND ALWAYS: keep v >= 129 */
  near(d.fragment.alpha_range[0], 128.5f / 255.f);
  near(d.fragment.alpha_range[1], 2.f);
  /* structure key: values do not matter, structure does */
  mh3_native_material_state t = s;
  t.konst[0].rgba[0] = 99;
  assert(mh3_shade_structure_key(&s) == mh3_shade_structure_key(&t));
  t.tev[0].color.b = 9;
  assert(mh3_shade_structure_key(&s) != mh3_shade_structure_key(&t));
}

static void fog(void) {
  mh3_shader_fog source;
  memset(&source, 0, sizeof source);
  source.a = 0.0125f;
  source.b = 2100000.f / 8.f;
  source.c = 0.3f;
  source.b_shift = 3.f;
  source.color[0] = 0.5f;

  for (uint32_t type = 2; type < 8; ++type) {
    mh3_shade_scene scene;
    mh3_shade_fog(type, &source, &scene);

    for (float depth = 0.05f; depth < 1.f; depth += 0.1f) {
      /* generator: ze = a * 2^24 / (b - (z24 >> shift)), f = clamp(ze - c) */
      const uint32_t z = (uint32_t)(depth * 16777216.f + 0.5f);
      const float ze = source.a * 16777216.f / (source.b - (float)(z >> 3));
      float f = ze - source.c;
      f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
      const float g0 = type == 6 || type == 7 ? 1.f - f : f;
      const float reference = type == 4   ? 1.f - exp2f(-8.f * f)
                              : type == 5 ? 1.f - exp2f(-8.f * f * f)
                              : type == 6 ? exp2f(-8.f * g0)
                              : type == 7 ? exp2f(-8.f * g0 * g0)
                                          : f;
      /* shader: per-vertex varying, fragment curve */
      float v =
          scene.fog[0] / (scene.fog[1] - depth * scene.fog[3]) - scene.fog[2];
      v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
      float g = scene.fog_curve[0] + scene.fog_curve[1] * v;

      if (scene.fog_curve[2] != 0.f)
        g *= g;

      const float amount = scene.fog_curve[3] * v + scene.fog_shape[0] +
                           scene.fog_shape[1] * exp2f(-8.f * g);
      assert(fabsf(amount - reference) < 0.002f);
    }
  }
}

/* The post SRT the vertex program builds equals mh3_texture_srt_calc's set
 * matrix for every identity-flag combination (the flags default the values). */
static void srt_rows(void) {
  for (uint32_t flags = 1; flags < 16; flags += 2) {
    mh3_pack_texture_srt slot;
    memset(&slot, 0, sizeof slot);
    slot.flags = flags;
    slot.scale[0] = (flags & 2) ? 1.f : 1.5f;
    slot.scale[1] = (flags & 2) ? 1.f : 0.75f;
    slot.rotation_degrees = (flags & 4) ? 0.f : 30.f;
    slot.translation[0] = (flags & 8) ? 0.f : 0.125f;
    slot.translation[1] = (flags & 8) ? 0.f : -0.25f;
    mh3_shade_gpu_srt srt;
    mh3_shade_gpu_srt_from_slot(&slot, &srt);
    float row0[3], row1[3];
    mh3_shade_gpu_srt_rows(&srt, row0, row1);
    /* XSI set matrix (80075F00 case 0, general form). */
    const float a = slot.scale[0] * cosf(slot.rotation_degrees * 0.017453293f),
                b = slot.scale[0] * sinf(slot.rotation_degrees * 0.017453293f),
                c = slot.scale[1] * sinf(slot.rotation_degrees * 0.017453293f),
                d = slot.scale[1] * cosf(slot.rotation_degrees * 0.017453293f);
    const float tu = slot.translation[0], tv = slot.translation[1];
    near(row0[0], a);
    near(row0[1], -b);
    near(row0[2], b - a * tu - b * tv);
    near(row1[0], c);
    near(row1[1], d);
    near(row1[2], -d - c * tu + d * tv + 1.f);
  }
}

static void color_mapped_text(void) {
  mh3_native_material_state s;
  base(&s);
  /* NW4R colour-mapped text: s0: prev = mix(c0, c1, texc), alpha mix(a0, a1,
   * texa); s1: prev = prev * rasc, alpha prev * rasa */
  stage(&s, 0, pass(2, 4, 8, 15, 0, 0), pass(1, 2, 4, 7, 0, 0), 0, 0, 0, 0);
  stage(&s, 1, pass(15, 0, 10, 15, 0, 0), pass(7, 0, 5, 7, 0, 0), 255, 0, 0, 0);
  const int16_t c0[4] = {85, 85, 85, 0}, c1[4] = {255, 255, 255, 255};

  for (unsigned i = 0; i < 4; ++i) {
    s.color[1].rgba[i] = c0[i];
    s.color[2].rgba[i] = c1[i];
  }

  mh3_shade_material m;
  assert(mh3_shade_classify(&s, &m) == (MH3_SHADE_EXACT | MH3_SHADE_FOLDED));
  assert(m.textures == 1 && m.model == MH3_SHADE_BASIC);
  const float tex[3][4] = {{0.75f, 0.75f, 0.75f, 0.75f}};
  const float black[4] = {0.f, 0.f, 0.f, 1.f}, tint[4] = {1.f, 0.5f, 0.25f, 0.5f};

  for (unsigned k = 0; k < 3; ++k) {
    const float mapped = c0[k] / 255.f + (c1[k] - c0[k]) / 255.f * 0.75f;
    check(&m, &s, 0, k, tex, black, 0.f);
    check(&m, &s, 0, k, tex, tint, mapped * tint[k]);
  }

  check(&m, &s, 1, 3, tex, black, 0.75f);
  check(&m, &s, 1, 3, tex, tint, 0.75f * 0.5f);
}

/* Effect particles' colour ramp: colour mix(c0, c1, texc) (or texa), alpha
 * a0 * texa. Its BASIC_LERP base and slope give the general BASIC result. */
static void particle_lerp(void) {
  for (unsigned replicate = 0; replicate < 2; ++replicate) {
    mh3_native_material_state s;
    base(&s);
    stage(&s, 0, pass(2, 4, replicate ? 9 : 8, 15, 0, 0), pass(7, 1, 4, 7, 0, 0), 0, 0, 12, 0);
    const uint8_t c0[4] = {200, 220, 240, 100}, c1[4] = {50, 60, 70, 255};

    for (unsigned i = 0; i < 4; ++i) {
      s.color[1].rgba[i] = c0[i];
      s.color[2].rgba[i] = c1[i];
    }

    mh3_shade_material m;
    assert(mh3_shade_classify(&s, &m) & MH3_SHADE_EXACT);
    assert(m.model == MH3_SHADE_BASIC && m.textures == 1 && !m.overlay);
    mh3_shade_gpu_material baked;
    mh3_shade_gpu_animated animated;
    uint8_t dynamic = 0;
    assert(mh3_shade_gpu_bake(&m, &baked, &dynamic) && dynamic);
    memset(&animated, 0, sizeof animated);
    mh3_shade_gpu_palette(&s, &animated);
    float lerp_base[4], slope[4];
    mh3_shade_gpu_lerp(&baked, &animated, lerp_base, slope);
    assert(baked.swizzle[2] == (replicate ? 1.f : 0.f));
    const float tex[3][4] = {{0.25f, 0.5f, 0.75f, 0.4f}}, ras[4] = {1.f, 1.f, 1.f, 1.f};

    for (unsigned k = 0; k < 4; ++k) {
      const float t = k < 3 && !replicate ? tex[0][k] : tex[0][3];
      const float expect = k < 3 ? (c0[k] + (c1[k] - c0[k]) * t) / 255.f : c0[3] / 255.f * tex[0][3];
      near(lerp_base[k] + slope[k] * t, expect);
      check(&m, &s, k == 3, k, tex, ras, expect);
    }
  }
}

int main(void) {
  /* std140 layouts of the material-GPU blocks (Material, Animated) */
  assert(sizeof(mh3_shade_gpu_pick) == 48);
  assert(offsetof(mh3_shade_gpu_material, pick) == 144);
  assert(offsetof(mh3_shade_gpu_material, add) == 144 + 9 * 48);
  assert(offsetof(mh3_shade_gpu_material, equation) == 144 + 9 * 48 + 32);
  assert(offsetof(mh3_shade_gpu_material, select) == 144 + 9 * 48 + 64);
  assert(offsetof(mh3_shade_gpu_material, swizzle) == 144 + 9 * 48 + 192);
  assert(sizeof(mh3_shade_gpu_material) == 784);
  assert(offsetof(mh3_shade_gpu_animated, alpha_range) == 144);
  assert(offsetof(mh3_shade_gpu_animated, srt) == 176);
  assert(sizeof(mh3_shade_gpu_animated) == 272);
  srt_rows();
  /* std140 offsets of the View and Draw blocks as declared in native_shader_source.cpp */
  assert(offsetof(mh3_shade_view, viewport) == 64);
  assert(sizeof(mh3_shade_view) == 80);
  assert(offsetof(mh3_shade_vertex, texgen) == 64);
  assert(sizeof(mh3_shade_texgen) == 112);
  assert(sizeof(mh3_shade_vertex) == 400);
  assert(MH3_SHADE_VERTEX_BYTES(1) == 176 && MH3_SHADE_VERTEX_BYTES(3) == sizeof(mh3_shade_vertex));
  assert(offsetof(mh3_shade_draw, fragment) == MH3_SHADE_FRAGMENT_OFFSET);
  assert(offsetof(mh3_shade_fragment, alpha_range) == 32);
  assert(offsetof(mh3_shade_fragment, shading) == 48);
  assert(MH3_SHADE_FRAGMENT_BYTES(MH3_SHADE_BASIC) == 48 + 544);
  assert(MH3_SHADE_FRAGMENT_BYTES(MH3_SHADE_MODULATE) == 48 + 112);
  assert(MH3_SHADE_FRAGMENT_BYTES(MH3_SHADE_LAYERS) == 48 + 176);
  assert(MH3_SHADE_FRAGMENT_BYTES(MH3_SHADE_CHAIN) == 48 + 432);
  assert(offsetof(mh3_shade_chain_uniforms, alpha_select) == 32 + 3 * 112);
  basic_texture_times_raster();
  konst_and_register();
  folded_multiply();
  color_mapped_text();
  particle_lerp();
  overlay_family();
  layers();
  sea_surface();
  cave_ice();
  city_wall();
  chain_masked_layers();
  chain_lit_glow();
  chain_alpha_map();
  split_color_alpha();
  approximations();
  fog();
  puts("mh3_shade: ok");
  return 0;
}
