# GPU skinning

## What the original does

GX has no per-vertex blending. Its transform unit holds a small matrix
memory; a shape's display list loads up to ten position matrices (and the
matching normal matrices) into it, and each vertex names one of them with
an 8-bit matrix index. A skinned MDL0 model therefore has one draw matrix
per node and one per envelope (a distinct combination of bone weights), and
the game computes every envelope on the CPU each frame from the model's
NodeMix byte code (`G3dCalcSkinning` 0x80074180, ported in
`runtime/src/g3d_calc_world.cpp`): a skin bone's matrix is its world matrix
times its inverse bind matrix, and an envelope is the weighted sum of those.
The view pass then makes each draw matrix view-space with its normal matrix.
A shape whose vertices use more than ten draw matrices is split into
batches, each loading its own ten.

## What the port does instead

Before GPU skinning, each batch was one native draw with a matrix palette
of position, normal and texture slots, so a skinned mesh drew once per
batch. Now:

- **Conversion** (`runtime/native/mh3_prepare_corners.c`,
  `mh3_prepare_skin.c`): a shape whose matrix selectors allow it is written
  skinned (mesh flag 16, M3MP v3). Each vertex carries four palette rows and
  four F32 weights from its draw matrix's NodeMix envelope (the four
  largest, renormalized, for the 13 envelopes on the disc with more than
  four), and the shape gets an M3SK blob listing its rows: rigid draw
  matrices, and skin bones with their inverse bind matrices.
- **Drawing**: the replay builds the mesh's palette once per draw from its
  pose version (`SkinPalette`, `runtime/src/model_scope.cpp`): a rigid row is
  the view-space draw matrix, a skin row the bone's view matrix times its
  inverse bind matrix. The vertex program blends three rows per influence
  and derives the normal matrix as the inverse-transpose; skinned programs
  are their own shader variant (`_skin`), so the shader bank holds a skinned
  copy of every mesh vertex program. The whole mesh is one draw.
- **Less CPU work**: a model none of whose shapes reads envelope matrices
  per vertex makes only the envelopes its rigid shapes use and only the
  views its draws read (Alatreon: 2 of 442 envelopes, 28 of 468 views).
- **Culling** tests the bounds under every row matrix
  ([culling](culling.md)).

## Why

In the Alatreon fight a frame had about 260 model draws from about 74
meshes, Alatreon's body mesh drew 101 times, and the Vita's render thread
spent about 70 µs per draw (`MH3_PROFILE=census`). Measured on the Vita in
the Alatreon fight:

- Drawing skinned meshes once: about 142 draws instead of 330, the render
  thread 14 ms per frame instead of 22.8, 48-49 fps instead of 41; the game
  thread became the limit (commit "Draw skinned model meshes once, blending
  bones in the vertex shader").
- Skipping the envelopes and views skinned models no longer read: the game
  thread's draw 4.6 instead of 5.7 ms per frame, 53.2 logical fps instead of
  49.7 (commit "Skip the envelope matrices skinned models no longer read").

On the GPU the skinned meshes are mostly vertex work: in the Alatreon fight
the main scene's TA work was 3.95 ms per frame with them and 1.87 ms without
(`MH3_PROFILE=skip=skinned`). Keeping the vertex programs in parallel mode and lighting far
lights from the eye cut it later ([shader specializations](shader-specializations.md)).

Rendered side by side, the batched and skinned forms agree to float error except where envelopes with
more than four influences were trimmed (up to about 1% of a model's radius
on em008).

## Example

The M3SK row record (version 1, little endian):

| Byte | Field |
| ---: | --- |
| 0 | matrix ID (U16) |
| 2 | kind (U8: 0 rigid, 1 skin bone) |
| 3 | reserved zero |
| 4 | inverse bind matrix, 12 F32, row-major 3×4 (zero for rigid rows) |

A vertex's matrix is `M = Σ weightᵢ · row(indexᵢ)`; the skinned Palette
block holds up to 255 rows of three `vec4` each.

## Applying it to another Wii game

Generic: every NW4R g3d model uses MDL0 envelopes and NodeMix, and every
GX game is limited to ten matrices per batch, so the same conversion and
shader apply to any game with skinned NW4R models. Check the influence
counts first (MH3: 2-4 per envelope except 13) and the number of rows a
shape needs against the vertex program's uniform budget (MH3: at most 56 skin
bones and 136 bones and rigid nodes per model; the skinned Palette block
allows 255 rows).

Game-specific: which CPU work can then be skipped. The port skips a model's
envelopes once its draw table shows that no shape reads them per vertex,
and only where no draw runs beside the pass: on the game thread while the
draw thread is idle, or on the draw thread during its scene graph walk
([pipelined draw](../threading/pipelined-draw.md)). Check who else
reads the envelope matrices before doing the same: a game whose effects or
collision read them must keep computing those.
