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
  return (dd + e[0] + e[2] * (a + (b - a) * c)) * e[1];
}
/* The classified model and the general BASIC program must both match. */
static void check(const mh3_shade_material *m,
                  const mh3_native_material_state *s, int alpha, unsigned k,
                  const float tex[3][4], const float ras[4], float expect) {
  mh3_shade_draw d;
  memset(&d, 0, sizeof d);
  mh3_shade_fill(m, s, &d);
  near(evaluate(m, &d, alpha, k, tex, ras), expect);
  mh3_shade_material basic = *m;
  basic.model = MH3_SHADE_BASIC;
  memset(&d, 0, sizeof d);
  mh3_shade_fill(&basic, s, &d);
  near(evaluate(&basic, &d, alpha, k, tex, ras), expect);
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

int main(void) {
  /* std140 offsets of the Draw block as declared in native_shader_source.cpp */
  assert(offsetof(mh3_shade_vertex, viewport) == 64);
  assert(offsetof(mh3_shade_vertex, texgen) == 144);
  assert(sizeof(mh3_shade_texgen) == 112);
  assert(sizeof(mh3_shade_vertex) == 480);
  assert(offsetof(mh3_shade_draw, fragment) == MH3_SHADE_FRAGMENT_OFFSET);
  assert(offsetof(mh3_shade_fragment, alpha_range) == 32);
  assert(offsetof(mh3_shade_fragment, shading) == 48);
  assert(MH3_SHADE_FRAGMENT_BYTES(MH3_SHADE_BASIC) == 48 + 544);
  assert(MH3_SHADE_FRAGMENT_BYTES(MH3_SHADE_MODULATE) == 48 + 112);
  basic_texture_times_raster();
  konst_and_register();
  folded_multiply();
  overlay_family();
  layers();
  split_color_alpha();
  approximations();
  fog();
  puts("mh3_shade: ok");
  return 0;
}
