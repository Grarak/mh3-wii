# Precomputed draws and shared uniform blocks

## What the original does

On the Wii a draw is a few register writes and a display list in the FIFO;
the matrices and material registers it needs stay loaded until something
changes them. There is no per-draw driver work on the CPU.

## What the port does instead

On GXM every draw normally sets its programs, vertex streams, textures and
uniform buffers through the context, and every mesh draw wrote a full
uniform block. Two changes cut that per-draw work on the render thread:

- **Precomputed draws** (`aurora-main/lib/gxm/renderer.cpp`,
  `draw_shaded_mesh`): each resident mesh draw (patched program, vertices,
  indices, count) has a record with a `SceGxmPrecomputedDraw`, vertex state
  and fragment state per GPU frame slot, in a 1 MiB uncached arena. A frame
  admitted to a slot has retired that slot's previous frame, so the render
  thread patches the slot's copies in place, only where streams, indices,
  uniform buffer addresses or textures changed, and draws with
  `sceGxmDrawPrecomputed`. Every other draw path unsets the precomputed
  states first. Records idle for 120 frames are swept; a full arena or any
  GXM error falls back to the context path (libgxm's "Precomputation",
  the SDK sample `tutorial_precomputation`).
- **A shared View block**: the projection and viewport live in their own
  block (GXM vertex `BUFFER[4]`, GL and WebGPU binding 6), written once
  into the frame's uniforms and shared by the draws until either changes.
  A mesh draw's Draw block is written only up to its last texture role's
  texgen (176 bytes for one role, 400 for three, instead of 512), because
  its program reads no other role.

## Why

Frame uniform memory is uncached CDRAM on the Vita (CPU writes run at about
650 MB/s), and libgxm's per-draw state setup showed up as the render
thread's largest library cost.

Vita, Jhen Mohran fight:

- Precomputed draws: render thread 15.30 → 14.51 ms per packet (model draw
  2.03 → 1.86, GL/GXM layer 2.42 → 2.27); libgxm 11.9% → 8.8% of the render
  thread's samples.
- View block and trimmed Draw blocks: model shading 2.15 → 2.06 ms per
  packet, about 2% fewer `memcpy` samples.

The displayed rate did not change in either case: the game thread limited
that scene.

## Example

The patch-in-place rule: a record keeps, per slot, the addresses it last
wrote into its precomputed states; a draw compares the current stream,
index, uniform and texture addresses with them and calls the matching
`sceGxmPrecomputed*Set*` only for those that differ.

## Applying it to another Wii game

Generic: any GXM renderer that draws the same meshes every frame. Keep one
precomputed state per frame slot so that patching never touches memory the
GPU may still read, and keep the context path as a fallback.

Game-specific: nothing beyond which draws are resident meshes. Split
uniform blocks by how often their contents change (per frame, per view, per
draw) and write only what the program reads.
