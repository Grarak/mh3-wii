# Compact native copies of hot guest data

## What the original does

Collision and land queries walk the game's own data structures. A ground
height query (`GetGroundHit2`, 0x802916C8, and `hit_ground_comon`,
0x80291D34) finds the point's grid cell, then reads each candidate triangle
of that cell's lists (about 21 in the Tundra) and runs three small tests per
triangle to see whether the point lies inside it. Every hunter, monster and
small creature asks every step. The land sweep (0x80293AEC) walks the grid
the same way for a moving sphere.

## What the port does instead

The queries are native ports (`runtime/src/ground_hit.cpp`,
`runtime/src/land_sweep.cpp`), and the ground query reads a compact native
copy of what it needs before it touches guest data:

- Per area, each candidate list is cached as its triangles' XZ bounding
  boxes in native memory, four to a group (struct of arrays).
- A query compares the point with four boxes in one NEON pass and reads and
  tests only the triangles whose box holds it, in list order (2-3 of about
  21 in the Tundra).
- Boxes are widened by the longest edge × 2^-16 / sin(smallest angle), past
  the triangle test's rounding, so skipping never changes a result. Thin
  triangles, unflagged triangles and far or NaN points take every
  candidate.
- A changed ground header (0x88 bytes) drops the cache.

The land sweep's math is NEON with approximations: edge distances are
compared squared, and normalizing and dividing use reciprocal estimates
with one Newton step (maximum relative error 1.6e-5). Its port check
compares with a tolerance (`MH3_PORT_CHECK=...,tolerance=1e-3`).

## Why

On the Cortex-A9 each triangle spanned two or three cache lines that mostly
missed, and the point lay outside the bounding box of 88% of the triangles
that reached the point-in-triangle test. The cost was memory latency, not
arithmetic.

Vita, Tundra: the ground queries took 8.2% of the game thread's samples
(1.3 ms a frame) before the boxes and 1.3% (0.12 ms a frame) after. Port
checks matched every call on the Tundra, the arena, the sandship and the
Volcano.

Software prefetching the next triangle while testing the current one gave
nothing measurable on the A9; shrinking what is read did.

## Example

`runtime/src/ground_hit.cpp`, the box walk:

```cpp
for (const Boxes::Group* end = group + count; group != end; ++group) {
    const uint32x4_t inX = vandq_u32(vcgeq_f32(x_, vld1q_f32(group->minX)), vcleq_f32(x_, vld1q_f32(group->maxX)));
    const uint32x4_t inZ = vandq_u32(vcgeq_f32(z_, vld1q_f32(group->minZ)), vcleq_f32(z_, vld1q_f32(group->maxZ)));
    uint64_t lanes = vget_lane_u64(vreinterpret_u64_u16(vmovn_u32(vandq_u32(inX, inZ))), 0) & 0x0001000100010001ull;

    for (; lanes; lanes &= lanes - 1)
        if (take(group->triangles[std::countr_zero(lanes) / 16]))
            return;
}
```

## Applying it to another Wii game

Generic: the pattern. When a hot native port still spends its time in cache
misses on guest structures, keep a small, native-endian, struct-of-arrays
copy of the fields the hot test reads, keyed by something cheap to check
for changes, and fall back to the original walk whenever the copy cannot
decide exactly.

Game-specific: which structures are read often and change rarely
(collision grids, static object lists, lookup tables), and what signals
that they changed. Widen conservative bounds past the original test's
rounding so that the result stays identical, and confirm with port checks
over several maps.
