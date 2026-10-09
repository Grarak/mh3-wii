# Culling models outside the view

## What the original does

MH3's scene queue (0x800E3E80 and its helpers) puts every listed scene
object into depth-sorted buckets. Its visibility test (0x800E41AC)
compares view depth with the object's near and far limits only, so the
game draws every model within its distance limits, also beside and behind
the camera. On the Wii the GPU's clipping discards those triangles after
transforming them.

## What the port does instead

Two native tests skip what produces no pixel; both are on by default
(scene queue,
renderer status):

- **Game thread** (`runtime/src/scene_cull.cpp`, C core
  `runtime/native/mh3_scene_cull.c`): after a plain scene model's pre-draw
  callback and before its draw (0x8004978C), each converted mesh's position
  box is tested under every view matrix a vertex of it can select, against
  the four side planes of the current projection, viewport and scissor. If
  one plane rejects every box, the draw is skipped; the callback still ran
  and the model's pack stays demanded. Models with draw callbacks, models
  without a certified draw table and meshes that cannot be bounded are always
  drawn.
- **Render thread** (`runtime/src/model_cull.cpp`): a mesh draw whose box
  is outside the drawable rectangle is dropped.

Both use the same test, `mh3_cull_boxes_outside`. A skinned mesh
([GPU skinning](gpu-skinning.md)) is tested under every palette row's
matrix, since a blended vertex is a convex combination of the rows'
transforms; the scene cull adds each row's bounds to its matrix's box.

## Why

On the Vita in Alatreon the two together skip 28-34% of scene models and
10-11% of mesh draws. Render thread per packet, one run each: 26.4 → 24.5 ms
without the material blocks, 24.4 → 23.9 ms with them; the game thread does
not change measurably (renderer status).
On the SGX, culling happens after vertex shading
([libgxm guide](libgxm-for-wii-ports.md#9-the-per-draw-budget)), so a draw
that is skipped on the CPU also saves its vertex work.

## Example

`runtime/native/mh3_model_cull.c`:

```c
int mh3_cull_boxes_outside(const mh3_cull_frustum *f, const float *const *matrices, const mh3_cull_box *boxes, unsigned count) {
    if (!f || !matrices || !boxes || !count)
        return 0;

    for (unsigned k = 0; k < 4; k++) {
        unsigned i = 0;

        for (; i < count; i++) {
            const float value = box_plane_max(f->plane[k], matrices[i], &boxes[i]);
            /* NaN compares false: never culled. */
            if (!(value < 0.f))
                break;
        }

        if (i == count)
            return 1;
    }

    return 0;
}
```

A model is culled only if a single plane has all of its boxes outside; a
NaN never culls.

## Applying it to another Wii game

Generic: the box test, and the idea of using the converted meshes' bounds
with the game's own view matrices, apply to any game with converted models.

Game-specific: where the game decides visibility and what may not be
skipped. Callbacks and objects that write state during their draw must
still run; here the test sits after the node's pre-draw callback, and
models with an enabled draw callback are never culled. Check a widened view
(a projection wider than the game's camera) to see that no object
disappears at the edges.
