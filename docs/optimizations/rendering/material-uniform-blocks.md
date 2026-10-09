# Baked material blocks in GPU memory

## What the original does

A GX material is register state: TEV stage configuration, colour registers
(c0-c3), konst colours (k0-k3) and their selectors, the alpha compare, blend
mode and texture matrices. NW4R writes all of it before each draw, and
animations (per-character colours, flashing, scrolling textures) rewrite
some of it every frame. The GPU reads it from registers; nothing is
compiled.

## What the port does instead

The fixed programs ([fixed shading](fixed-shading-models.md)) take a
material's constants from two uniform blocks instead of resolving each
coefficient on the CPU per draw:

- **Material** (784 bytes, std140): a pure function of the classified
  structure (TEV stages, swap tables, alpha compare functions). It holds
  each coefficient's static factor and a *pick*: which colour register or
  konst colour supplies the animated factor, and which component. It is
  baked once per structure (`mh3_shade_gpu_bake`), uploaded into persistent
  GPU memory at its first draw (a 512 KiB CDRAM chunk on GXM, one UBO on
  GLES) and shared by every draw of that structure. A changed structure is
  another record; nothing is rebaked.
- **Animated** (272 bytes): the values that change at run time: the TEV
  and konst colours, the alpha compare as an interval, the LOD bias and each
  texture role's SRT. The render thread reuses the previous draw's block
  when the material, texture and SRT revisions are unchanged, and writes a
  new one into the frame's uniform memory only when its bytes differ.

The fragment program multiplies the static factor by the picked register.
Programs whose picks would push work from the per-draw (secondary) program
into the per-fragment one (BASIC, and two- and three-texture MODULATE with
fog) get the coefficients resolved on the CPU instead.

## Why

- On the Vita the render thread took 24.4 ms per packet instead of 26.4
  with the blocks in Alatreon (23.9 instead of 24.5 with culling on); the
  game thread did not change measurably.
- psp2shaderperf shows the picks free for one-texture MODULATE (11 cycles
  against 12) because the Cg compiler moves uniform-only arithmetic into the
  secondary program, which runs once per draw.
- 10057/6 uses 11 material records (8.4 KiB), Alatreon 21; on arm64 Linux in
  10057/6 the Animated block was reused for 98.5 of 187 draws per frame,
  found equal for 22.0 and written for 66.8.

## Example

The pick in Cg (`runtime/src/native_shader_source.cpp`):

```c
real4 coefficient(int k) {
#if DYNAMIC
    Pick p = material.pick[k];
    real4 c = anim.palette[int(p.index.x)];
    real4 a = anim.palette[int(p.index.y)];
    return material.coefficient[k] * real4(lerp(c.rgb, dot(c, p.color_mask).xxx, p.index.z), dot(a, p.alpha_mask));
#else
    return material.coefficient[k];
#endif
}
```

A development check compared, for every draw, the coefficients the
programs derive (a C mirror, `mh3_shade_gpu_resolve`) with the CPU path
(`mh3_shade_fill`): 880,000 draws in Alatreon, no mismatch.

## Applying it to another Wii game

Generic: splitting material state into an immutable per-structure block and
a small per-draw animated block works for any fixed-program renderer of GX
materials, and the bake follows TEV semantics.

Game-specific: which values animate. Take a census of the material fields
written at run time (here the konst colours per character instance, alpha
compare, blend and texture SRT) before deciding what goes in the animated
block. Check psp2shaderperf for every program with picks: a pick that the
compiler cannot move into the secondary program costs per fragment.
