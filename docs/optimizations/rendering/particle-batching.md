# Particle effects: native strategies and batched draws

## What the original does

MH3's particles are NW4R `ef` effects. Each owner calls `Effect::Calc`
(0x800A4C20) from its own update; the particle managers advance emitters
and particles (`ParticleManager::Calc` 0x800AC35C) and evaluate animation
curves. At draw time a draw strategy per manager (billboard, directional,
free, stripe, line, point) sets the GX material and TEV colours, then emits
each particle's quad as immediate GX vertices; a particle with a different
colour is a new TEV register write in the same stream.

## What the port does instead

- **Native producers** (`runtime/native/mh3_effect_draw.c`,
  `runtime/src/native_effect_draw.cpp`): every strategy's traversal and
  quad leaves, the common material setup and texture animation, and the THP
  movie draw, written into frame allocations
  (effects port).
- **Native simulation**: `Effect::Calc`, `ParticleManager::Calc`, the curve
  executors, the particle accessors and the draw order are ported; the
  variable step evaluates curves between ticks
  ([variable step](../simulation/variable-step.md)).
- **Batching**: a frame-packet record of material geometry joins the record
  before it when both have the same draw, material, colour, lighting and
  texture snapshots and their vertices follow each other in the packet's
  arenas. The result is one triangle list in submission order, so blending,
  depth and alpha test see the same primitives in the same order: merging
  adjacent same-state draws is exact. Up to 65,536 vertices per draw.
- **Particles of different colours drawn as one**: particles that fade on
  their own set their own colour registers, so each recorded a draw of its
  own (the base camp waterfall's spray: 138 particle draws a frame). On the
  render thread, consecutive quad records that differ only in their colour
  snapshots (the same draw, material, lighting and texture snapshots, normal
  and colour, the vertices following on) draw as one when the material is a
  one-texture lerp (BASIC with constant a, b and d and c from texture role 0,
  the particles' colour ramps). Each quad's base and slope
  (`mh3_shade_gpu_lerp`, what the `_lerp` program computed from uniforms once
  per draw) go with its vertices in a second stream (`AuroraNativeQuadLerp`,
  32 bytes a corner), and the `_vlerp` fragment program takes them from there:
  `clamp(base + slope · texel)`. The run binds the baked Material block
  instead of a resolved copy per particle, so per particle the render thread
  writes 128 bytes instead of an Animated block, a resolved Material block and
  the Quad and Draw blocks (about 1.7 KiB, CDRAM on the Vita), and makes no
  draw call. Primitive order is unchanged; a run whose parts differ in their
  alpha comparison draws record by record (`DrawNativeQuadRun`,
  `QuadRunEnd`). On the Vita the `basic_t1_vlerp_gpu` fragment program takes
  6 cycles against `basic_t1_lerp_gpu`'s 7 (psp2shaderperf), and the quad
  vertex program 87 against 83, run four times a quad.
- **Colours as their own component**: the TEV colour registers, konst
  colours and alpha references (108 bytes, `NATIVE_STATE_COLORS`) change
  without touching the 1.3 KiB material snapshot, so a particle with new
  colours stores 108 bytes and references the unchanged material.
- **Compact quads the GPU reads in place**: a plain billboard quad was four
  144-byte pre-transformed vertices that the render thread copied into GPU
  memory, its biggest copy. Within one draw only the corners and their two
  texture coordinates differ, so a quad records 36-byte vertices
  (object-space corner and coordinates) straight into the packet's quad
  arena, which on the Vita is uncached main memory mapped for GXM; the GPU
  draws from it in place with one static index pattern. The draw's matrices,
  normal and colour go in a 160-byte uniform block and the vertex program
  (`NativeShaderVertex::Quad`) does the rest.
- **Setups that change nothing are skipped**: emitters drawn one after
  another often publish the same material; a strategy's setup keeps the
  inputs it last published and, while they and the material revision are
  unchanged, redoes only its guest writes and the fog (about 60% of setups
  in the village).
- **Free-strategy matrices on the host**: the free strategy's quads called
  two guest functions per particle (rotation 0x800BEA64, quad matrix
  0x800BE99C); both are host math now, bit-identical on 24,576 village quads.
- **Uniform blocks built before they reach CDRAM**: a quad's Draw block and
  a resolved material are built on the stack and copied once, instead of
  cleared and patched in uniform memory (CPU writes to CDRAM run at
  uncached speed).
- **The calculation runs beside the draw** on the guest worker
  ([guest worker](../threading/guest-worker.md)).

## Why

Particles took about a third of the Vita game thread in Alatreon (the
simulation about 19%, the billboard draws about 14%). With the ports and
batching, the Pi 5 game thread (Cortex-A9 build, Alatreon, moving, three
alternating runs) went from 4.29-4.58 to 4.04-4.08 ms per frame, effects
from 1.35 to 1.00 ms; on arm64 Linux 45-48% of material geometry submissions in
effect-heavy phases join the draw before them
(effects port). The Vita gain
was not measured separately.

The draw-side changes, measured on the Vita in the village:

| Change | Before | After |
| --- | ---: | ---: |
| Compact quads | material geometry 3.28 ms a packet, render-thread copies 142 MB per 300 packets, 57.4 fps shown | 2.38 ms, 121 MB, 58.8 fps |
| Skipped setups | billboard setup 12% of the draw thread, 13.60 ms a frame, 69.1 logical fps | 6%, 12.81 ms, 72.9 fps |
| Free-strategy matrices | effect draw 5.40 ms a frame | 5.07 ms |
| Blocks built before CDRAM | `DrawNativeQuads` 13.3% of the render thread, 16.44 ms a packet | 11.0%, 16.20 ms |

Particles of different colours drawn as one, on the Pi 5 (arm32 Cortex-A9
build, frame limit off) in the Deserted Island base camp facing the
waterfall: 138 particle draws a frame became 21 (146 quad records in 19
runs a packet); the render thread's CPU went from 7.28 to 5.61 ms a packet
(from 99% to 69% busy, perf), less than looking away from the waterfall
before (6.13 ms), and the frames drawn from 89 to 93.5 a second. The game and
draw threads did not change. The waterfall had cost the render thread 1.15 ms
more than the view away, nearly all of it in the driver and buffer uploads
of the separate draws; its particles cover only 0.7 screens (`MH3_PROFILE=fill`).

## Example

The merge condition, from
effects port: the record joins
the previous one when both are material geometry with the same draw,
material, colour, lighting and texture snapshots, and the new vertices and
indices follow the previous ones in the packet's arenas. Pre-transformed
geometry reads no post texture matrices, so those are neither compared nor
captured for it. Particles whose colours differ join at replay instead, when
their material is a lerp: their colours become each quad's base and slope.

## Applying it to another Wii game

Generic: many Wii games use NW4R `ef`; its managers, strategies and curve
executors are library code (at other addresses in each game, and versions
differ), and the adjacent-merge rule applies to any immediate-mode geometry
recorded in order.

Game-specific: which strategies the game uses, and how its effects reach
their owners' state (MH3's owners set each effect's root matrix and some
read its state right after calculating it). Measure the share of draws that can merge before
building it: the gain depends on how many consecutive particles share a
state.
