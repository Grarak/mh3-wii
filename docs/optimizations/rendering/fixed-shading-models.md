# Fixed shading models instead of TEV

## What the original does

GX colours each pixel with the TEV, up to 16 combiner stages. Each stage
takes four inputs (a, b, c, d) from texture samples, the rasterized vertex
colour, constant ("konst") colours or the previous stages' registers, and
computes `(d ± mix(a, b, c) + bias) · scale` with optional clamping, for
colour and alpha separately, with swap tables, indirect texturing and an
alpha compare after the last stage. A material's TEV program is plain
register state set before the draw; NW4R g3d writes it from the model's
material records (MDL0) and animations change its colours.

## What the port does instead

Every material is classified once, by its combiner structure, into one of
three hand-written programs (`runtime/native/mh3_shade.h`):

- **MODULATE**: `((t0·w0 + t1·w1 + offset [+ overlay·t1·t2]) · (v·kv + kc) + d) · s`,
  per channel: texture times lit vertex colour, layered textures, and the
  four-stage "base plus masked overlay" family.
- **BASIC**: the general three-texture `(d + bias ± mix(a, b, c)) · scale`
  for colour and alpha separately, as a fallback; optionally the saturated
  result times the lit vertex colour (the Loc Lac city walls,
  `ras · 2 · mix(c0, t0·k, t1)`: a baked shadow mask blends toward the shadow
  colour register before lighting).
- **LAYERS**: the multi-stage sums neither folds,
  `sat(sat((Σ_r (v·kv_r + kc_r)·t_r + v·kv_3 + kc_3) · s) · (v·mv + mc))`:
  each stage adds a texture times the vertex colour or a constant, then at
  most one multiply (the Deserted Island sea surface, whose alpha BASIC had
  reduced wrongly: the sea showed the seabed).

`runtime/native/mh3_shade.c` folds multi-stage programs into these forms
where that is exact (multiplies, up to three added layers, stages that
leave the previous value alone; when those fall back, also `mix(A, prev, C)`
over a single product and a closing multiply by the vertex colour); a structure that has no exact form gets
the nearest model, and development builds log it. Per draw only the
constant references (TEV registers, konst colours) are resolved, or read by
the GPU from a material block ([material uniforms](material-uniform-blocks.md)).

The programs are written twice, GLSL ES 3.0 and Cg, in
`runtime/src/native_shader_source.cpp`; variants are compile-time defines
(textures 0-3, overlay, alpha test, fog, lighting: none, 1-8 spot lights
or general; a texgen kind per texture role), plus specializations
([shader specializations](shader-specializations.md)). The
vertex stage computes lighting and the whole fog amount (the clamped
distance, the curve and the clamp), which the fragment stage blends by
(commit "Compute fog per vertex and light far lights from the eye"); GX's
alpha compare becomes a discard interval.

## Why

- **Coverage is concentrated.** A survey of every MDL0 material on the disc
  (1,522 BRRES files, 10,482 materials) found 92 distinct combiner
  topologies; one-stage materials are 92%, and the top three topologies
  cover 92.5%. On the 45 packs of the Alatreon quest (3,549 materials) the
  classifier reproduces 97.6% exactly and MODULATE covers 98.6%.
- **Cost.** psp2shaderperf estimates: the fixed MODULATE program with one
  texture 10 cycles per fragment, against 46-175 for the TEV-generated
  programs it replaced; the common vertex programs 56.5 (unlit) to 185 (four
  spot lights) against 271-848 (the general lighting and texgen variant
  still costs 848).
- **A known program set.** A finite set of programs can be compiled ahead of
  time for the Vita ([shader bank](offline-shader-bank.md)); a generator
  that emits one program per TEV configuration cannot.

## Example

The MODULATE fragment in Cg (`runtime/src/native_shader_source.cpp`):

```c
// Keep each arithmetic chain two components wide. The float4 chain's
// masked upper-lane uniform MAD loses B/A on the Vita; identical RG/BA
// equations pass the isolated native Shade-buffer readback test.
real2 base_rg = t0.rg * w0.rg + offset.rg;
real2 base_ba = t0.ba * w0.ba + offset.ba;
...
real4 result;
result.rg = saturate((base_rg * (v.rg * kv.rg + kc.rg) + d.rg) * s.rg);
result.ba = saturate((base_ba * (v.ba * kv.ba + kc.ba) + d.ba) * s.ba);
#if ALPHA_TEST
if (result.a < ALPHA_RANGE.x || result.a >= ALPHA_RANGE.y)
    discard;
#endif
```

GX's alpha compare (two comparisons and an AND/OR/XOR combination) becomes
the interval `[lo, hi)` (`alpha_range` in `runtime/native/mh3_shade.c`):
AND intersects the two intervals, OR takes their hull (exact when they touch
or one is empty), and XOR/XNOR keep only the first comparison, an
approximation.

## Applying it to another Wii game

Generic: the TEV semantics, the two programs, the alpha-compare and fog
conversions, and the classifier's folding rules. A game that uses NW4R g3d
materials has the same material records.

Game-specific: the distribution of combiner structures. Survey every
material on the disc first (offline, from the model files); if a few
topologies cover nearly everything, fixed programs plus a general fallback
work. A game with heavy multi-texture effects (water, indirect warps, many
TEV stages) needs more models or a different fallback. Keep a reference
renderer (here the TEV path, until it was removed) and compare frame dumps
while classifying.
