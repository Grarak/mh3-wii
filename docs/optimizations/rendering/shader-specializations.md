# Shader specializations

## What the original does

GX evaluates fog, lighting, texture coordinate generation and the combiner
in fixed hardware: a linear fog curve costs the same as an exponential one,
per-pixel fog the same as none, an identity texture matrix the same as a
general one, and writing RGB with alpha masked costs nothing extra. A shader
that reproduces the general case pays for every term on every fragment or
vertex.

## What the port does instead

Compile-time variants and cheaper forms of the fixed programs drop work a
draw does not need. Most are exact; the others trade a bounded difference
for the cycles (last column):

| Variant | Applies when | Effect | Exact |
| --- | --- | --- | --- |
| Identity/Affine texgen | the post matrix is exactly identity | no matrix product and divide | yes |
| `_pure` | MODULATE with one texture, literal zero offsets and one scales | `t0 · v` directly | yes |
| `_lerp` | BASIC with constant a, b and d and c from texture 0 (the effect particles) | base and slope from uniforms, which GXM's secondary program evaluates once per draw; one multiply-add per fragment | the same equation rearranged; GLES frames of the Alatreon fight within 3 levels |
| `_vlerp` (with `quad_*_lerp`) | a run of `_lerp` particle quads that differ only in their colours | each quad's base and slope from its vertices; one draw for the run | the same base and slope, computed on the CPU in single precision |
| `_a1` (GXM) | an unblended RGB-only draw to a target certified to hold alpha 1 everywhere | writes A=1 with a full RGBA mask, which keeps hidden-surface removal on | yes |
| half-precision colours (GXM) | all Cg fragment programs except the ones with dynamic picks | 11 significant bits instead of 24 | at most one 8-bit step |
| Fog per vertex | every fogged draw | the vertex stage computes the whole fog amount (clamped distance, curve, clamp); the fragment blends by it | exact for linear curves where a triangle's corners are all inside the fog range; across its ends the clamp is taken at the corners |
| Far lights | every light of a draw is an unattenuated GX spot light at least 1e6 units from the eye | lights by the direction from the eye through a uniform branch | at most about a degree off for a vertex 2e4 units away |

`_fog_linear` (no `exp2` term for GX fog types with `(type & 7) < 4`)
remains in the key, but since fog moved to the vertex stage its fragments
are identical to the general ones.

On the Vita a vertex program must also stay in parallel mode (four vertices
per USSE thread): psp2cgc compiled three selects of the mesh vertex program
(the normal's conditional normalize, the skinned matrix's
singular-determinant fallback, the projective texgen divide) as dynamic flow
control, which runs per instance. They are written as scalar selects and
lerps now, with the same results (commit "Keep the Vita mesh vertex
programs in parallel mode"). The far-light branch is on a uniform, which
keeps parallel mode.

## Why

GPU time on the Vita is fragment-bound in the fights (a GPU capture of the
Alatreon fight: the 3D scene's fragment jobs 68% USSE and 9%
texture activity). Measured on the Vita:

- `_a1`: desert quest GPU work per frame 20.89 → 18.53 ms, the main
  framebuffer's 3D 13.44 → 11.07 ms. Sony's libgxm overview says colour
  masking prevents hidden-surface removal.
- Half precision together with CPU-resolved picks: Alatreon GPU work per
  frame 19.06 → 16.53 ms.
- Fog per vertex: psp2shaderperf `modulate_t1_pure_fog_linear` 12 → 7
  cycles; Alatreon, two runs each, the main scene's 3D 10.70/11.01 →
  8.75/9.12 ms and the GPU busy 17.49/18.13 → 15.76/16.47 ms per frame
  (commit "Compute fog per vertex and light far lights from the eye").
- Far lights: the Alatreon body's skinned three-light vertex program 254.5 →
  187.5 cycles on that path, the main scene's TA 3.48 → 3.30 ms; the GPU busy
  did not change, as vertex work overlaps fragment work (same commit).
- Parallel mode: the spot-lit vertex program 153 cycles per instance →
  146.75 parallel, the main scene's TA 3.9 → 3.5 ms per frame; the frame rate
  did not change, the game thread still limited it.
- `_lerp`: `basic_t1_lerp_gpu` 13 → 7 cycles, but no change of the 3D scene's
  time (8.75/9.12 → 8.67/9.22 ms): the particles' cost is not their
  arithmetic (commit "Fold the one-texture lerp equation into a
  multiply-add").

## Example

The certificate that makes `_a1` exact: the frame starts with a full clear to `{0,0,0,1}`; RGB-only writes and
certified A=1 writes keep the certificate; any other alpha write, a partial
copy from an uncertified source, a CPU upload or a scissored clear drops it.
An unblended RGB-only draw to a certified target can then write alpha 1,
which is the byte already there.

## Applying it to another Wii game

Generic: the identity texgen, destination-alpha, fog-per-vertex and
far-light forms follow from GX semantics and apply to any game whose scenes
use those cases (MH3's directional lights are unattenuated GX spot lights
millions of units away; a game that lights the same way can use the
far-light path). Find candidates by counting, per draw, which terms of the general
program are constant (a census of the material and fog state over a few
minutes of play), and check each variant against the general program over
its whole input range before using it (an exhaustive or randomized
comparison of the two programs' C mirrors).
Measure GPU time, not shader cycles: `_a1` gains through hidden-surface
removal, which psp2shaderperf cannot show, and `_lerp` saved 6 cycles per
fragment without a measurable gain. Check every new vertex program's mode
in psp2shaderperf: one dynamic branch makes it per instance.
