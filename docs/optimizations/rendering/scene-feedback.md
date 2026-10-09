# Scene feedback on the GPU instead of EFB copies

## What the original does

GX renders into the embedded framebuffer (EFB) and copies it out: to the
display (XFB) and, with `GXCopyTex`, into textures in RAM that later draws
sample. MH3 uses copies for its scene feedback: temporal history (an
RGB565 copy of the previous frame), the glare (`GlareFilter::draw`
0x80056F88 and its bright pass 0x80057354: repeated 2× box downsampling,
luminance weights 77/150/29, thresholds, then a composite), saved-scene
captures for pause and menus, and the character texture compositor. Some
of it also reads and writes the EFB from the CPU: the glare's bright pass
waited in `GXDrawDone` and rewrote the downscaled scene through about 9,000
EFB peeks and pokes per frame, and three colour menus pick a colour with
`GXPeekARGB`.

## What the port does instead

Every feedback pass draws into native GPU render targets
(`runtime/native/mh3_scene_feedback.c`, `runtime/src/native_scene_feedback.cpp`;
scene feedback):

- A guest texture address that a copy wrote names a GPU producer identity
  and revision, not RAM bytes. Captures and the bright pass draw into fresh
  targets; a replacement is a new revision; sampled older revisions keep
  their backing until the GPU's fence for them completes. Nothing is read
  back.
- The glare keeps its authored downsampling, weights and thresholds, in
  floating point instead of the CPU's byte arithmetic. Its exact 2:1
  levels (160×112 and 80×56) take one bilinear tap at each 2×2 block's
  shared corner instead of four, the same weights (commit "Sample exact
  glare halvings once").
- The colour menus sample the original texture bytes and sprite geometry on
  the CPU instead of peeking the EFB.
- Targets are pooled and reused after their last reference and GPU
  retirement (target reuse).
- The glare no longer draws its capture back onto the frame: the original
  did that because its bright pass used the EFB as scratch space, which the
  native bright pass does not.

## Why

- CPU access to the framebuffer serializes the CPU and the GPU and blocks
  frame skipping. The first port of the glare removed the CPU pass and its
  9,000 peeks and pokes per frame (commit "Remove MH3's bloom with a C port
  of GlareFilter::draw").
- A copy to RAM costs a GPU-to-CPU transfer per frame; MH3 only samples its
  copies as textures (a page-protection trace saw no CPU reads of copy
  destinations in the village), so they stay on the GPU.
- Dropping the glare's draw-back: Vita Alatreon fight, the post-capture
  scene 3,172 → 1,793 µs of 3D time per frame, GPU busy 20.8 → 18.9 ms per
  frame (commit "Skip the glare's draw-back of the scene it has just
  captured").
- One tap for the exact halvings: those levels 0.25 → 0.20 ms per frame
  (GPU capture).

## Example

From the entry-point table of scene feedback:

| Address | Native behaviour |
| --- | --- |
| 80055FBC | temporal history: additive and subtractive modes, crop/offset, frame counter |
| 80057354 | GPU downsample and brightness pass, replacing all DrawDone/Peek/Poke work |
| 8005876C | conditional publication of the current scene |
| 80058F84 | current-scene capture into the saved identity |

## Applying it to another Wii game

Generic: treating a copy destination as a GPU resource identity (address
plus revision) instead of bytes, and the render-target pool with
fence-based retirement, work for any game.

Game-specific: whether the game reads copies on the CPU. Trace it first: a
page-protection trace of copy destinations (the `MH3_ENABLE_EFB_COPY_TRACE`
CMake option here) shows CPU reads; each one must be replaced by CPU work on the source
data (as the colour menus' sampling does) or kept as a readback. Passes
that used the EFB as scratch space often have steps that only made sense on
the Wii, like the draw-back here; check each step against what the native
version leaves in its targets.
