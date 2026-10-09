# libgxm for Wii ports

How to drive the PS Vita's GPU through libgxm when the game was written for
the Wii's GX, as this port does. It maps each GX concept onto what this
repository does on GXM, says why, and gives the numbers measured here. It
is not a libgxm tutorial: Sony's SDK documents describe the API, and a
general guide to writing a native GXM renderer (see "Sources") covers the
hardware rules this page relies on.

Measurements are from 2026-10-05 and earlier unless dated.

## Sources and evidence tags

- Code in this repository: the GXM backend `aurora-main/lib/gxm/` (device,
  renderer, memory, draw memory, textures, raster, shader bank), the frame
  ring `aurora-main/include/aurora/gpu_completion.hpp`, the native renderer
  in `runtime/src/native_*` and `runtime/native/`.
- This repository's notes: the other pages under `docs/optimizations/`.
- Sony's libgxm documentation and the SDK samples' guidance for the SGX543.

Tags:

- **[code]**: read from the file named.
- **[doc]**: stated in this repository's note named (the notes it drew on
  were folded into the pages under `docs/optimizations/`).
- **[devkit]**, **[Pi 5]**, **[arm64]**: measured on that machine (a PS Vita
  development kit, a Raspberry Pi 5 running the Cortex-A9 build, an arm64
  Linux build with software rendering), with the source in brackets (a note or a commit subject).
- **[psp2cgc]**: a psp2shaderperf estimate of a program compiled by Sony's
  compiler; cycles, not device time.
- **[sdk]**: from Sony's documentation; not measured in this repository.

## 1. The shape of the problem

GX is a fixed-function pipeline fed through one FIFO: the CPU writes
register loads (TEV combiner stages, the transform unit's matrices and
lights, blending, depth) and vertex data or display lists, and the GPU
consumes them in order into the embedded framebuffer (EFB). Nothing is
compiled, state is cheap to change, the CPU can read the EFB back, and
copies out of the EFB are the only way to render to a texture.

libgxm drives a tile-based deferred renderer (the SGX543MP4+). Its unit of
work is a scene: a tiling job over one render target, whose tiles are
loaded, shaded and stored as a whole. Programs are compiled GXP binaries
that the shader patcher combines with vertex formats and blend state. All
GPU-read memory is the application's own, read in place.

So a Wii port does not translate GX calls into GXM calls. It records what
the game draws in typed form and replays it on GXM with a small set of
programs ([GX replacement](gx-replacement.md)). The table below is the map
this port uses; the sections after it explain each row.

| GX | What it is on the Wii | On GXM in this port | Section |
| --- | --- | --- | --- |
| The FIFO and display lists | command stream, decoded by the GPU | frame packets of typed records, replayed by a render thread; indexed U16 triangle lists | 2 |
| TEV stages, konst colours, swap tables | up to 16 combiner stages of register state | two fixed shading models in an offline-compiled bank; constants in uniform blocks | 3, 4 |
| XF matrix memory, matrix index per vertex | ten position/normal matrices loaded per batch | a per-draw palette uniform block; skinned meshes blend bones in the vertex program | 8 |
| XF lights and channels | per-vertex colour lighting | vertex program variants (none, 1-8 spot lights, general), Scene block | 3 |
| Texture coordinate generation, post matrices | XF texgen | vertex program texgen kinds per role; animated SRT in a uniform block | 3, 4 |
| Texture formats (CMPR, CI4/CI8, I4/I8/IA4/IA8, RGB5A3...) | tiled formats read from RAM | converted at load time into native formats in CDRAM | 5 |
| Fog | per-pixel fog from depth | the fog amount computed per vertex, blended per fragment | 3 |
| Alpha compare | two comparisons and an operation after the TEV | a `discard` outside an interval `[lo, hi)` | 3 |
| Z compare before texture | early depth test and write | GXM tests depth before shading; depth write with discard is not supported | 3 |
| Blending, logic ops | blend mode register | a patched fragment program per blend state; of the logic ops only COPY and NOOP | 3 |
| Scissor | per-pixel scissor | mask-update draws (pixel exact) | 6 |
| EFB copies (`GXCopyTex`), CPU EFB access | copy to a texture in RAM; peeks and pokes | render targets sampled as textures; no readback | 6 |
| Clears (copy clear, clear quads) | part of the copy | a clear draw; a full depth clear at a scene's start becomes the background depth | 6 |
| `GXDrawDone`, draw sync | CPU waits for the GPU | scene notifications polled without waiting | 7 |
| VI, XFB, retrace | external framebuffer flipped at retrace | three display buffers, sync objects, the display queue | 7 |

## 2. Recording: packets on one thread, GXM on another

During the game only one thread calls libgxm: the render thread, on core
1, owns the context, the shader patcher and every allocation the GPU reads [doc:
renderer status, "GXM rules"]. The game thread (core 2) records a frame
packet: model items, material geometry, clears and scene feedback
operations, with draw state captured per component only when it changed
([render thread](../threading/render-thread.md)); the 3D scene's part is recorded by a
draw thread on core 3 while the game thread runs the next logic step
([pipelined draw](../threading/pipelined-draw.md)). Geometry is written once into the
packet's arenas; on the Vita the render thread copies each draw's geometry
into the GPU frame slot, because GXM reads geometry in place and the packet
arena is reused [doc: render thread].

Why a separate thread: the GXM calls, uniform writes and model preparation
moved off the game thread, which is the Vita's limit. On the Pi 5 the same
change took Alatreon from 49 to 145 game frames per second [Pi 5: commit
"Move drawing and present to a render thread fed by frame packets"]. The
devkit's render thread spent about 70 µs per model draw
[devkit: [gpu-skinning.md](gpu-skinning.md), `MH3_PROFILE=census`], so its
time grows with the number of draws (sections 8 and 9).

Draw calls are always indexed U16 triangle lists (`sceGxmDraw` with
`SCE_GXM_PRIMITIVE_TRIANGLES`, `SCE_GXM_INDEX_FORMAT_U16`) [code:
`renderer.cpp`, `draw_memory.cpp`]. GX's quads, strips and fans are
converted when the mesh is converted or the geometry is written; there is
no primitive restart on GXM [sdk].

## 3. Shading GX materials with fixed programs

**Do not generate a program per TEV configuration.** The first renderer did
(GLSL generated from TEV state); its fragment programs cost 46-175 cycles
and its vertex programs 271-848 [psp2cgc: commit "Shade native draws with
fixed hand-written programs instead of TEV"], and a retail Vita cannot
compile new programs at run time (section 4). A survey of every material on
the disc found 92 combiner topologies, three of them covering 92.5% [doc:
render plan]. The port classifies each material once into:

- **MODULATE**: `((t0·w0 + t1·w1 + offset [+ overlay·t1·t2]) · (v·kv + kc) + d) · s`;
  the plain one-texture form costs 8 cycles in half precision (12 in
  float) [psp2cgc: shader specializations], the pure one-texture form with
  fog 7 since the fog amount comes from the vertex stage [psp2cgc: commit
  "Compute fog per vertex and light far lights from the eye"].
- **BASIC**: `(d + bias ± mix(a, b, c)) · scale`, the general fallback, 51-97
  cycles [psp2cgc]. When a, b and d are constants and c is texture 0 (the
  effect particles), the equation is affine in the texel: the program
  computes base and slope from uniforms only, which GXM's secondary program
  evaluates once per draw, and does one multiply-add per fragment, 13 → 7
  cycles. The 3D scene's time did not change [psp2cgc, devkit: commit "Fold
  the one-texture lerp equation into a multiply-add"].

How each GX feature lands in the programs (`runtime/src/native_shader_source.cpp`,
`runtime/native/mh3_shade.c`) [code]:

- **Alpha compare** becomes `if (a < lo || a >= hi) discard;`. GX's two
  comparisons and AND/OR/XOR/XNOR become one interval: AND intersects, OR
  takes the hull (exact when the intervals touch or one is empty), XOR and
  XNOR keep the first comparison (an approximation). The bounds are
  placed half a step from the 8-bit thresholds, so the float compare
  matches GX's integer one.
- **Fog**: the vertex program computes the whole fog amount (the clamped
  eye distance, the curve, the clamp) and the fragment program mixes towards
  the fog colour by the interpolated amount. For the game's linear curves
  that is exact where a triangle's corners all lie inside the fog range;
  across the range's ends the clamp is taken at the corners. Alatreon's main
  scene: 3D 10.70/11.01 → 8.75/9.12 ms per frame, GPU busy 17.49/18.13 →
  15.76/16.47 ms [devkit: commit "Compute fog per vertex and light far lights
  from the eye"]. Before that, a linear fragment variant without the `exp2`
  term had saved 6 of 25 cycles for one texture.
- **Lighting and texgen** are vertex program variants: 0-8 spot lights or
  general lighting, and per texture role none, identity, affine, SRT or
  general texgen. An identity post matrix drops the matrix product and the
  divide: unlit one-role vertex programs 121 → 55.5 cycles [psp2cgc]. MH3's
  lights are unattenuated GX spot lights millions of units away; when all of
  a draw's lights are at least 1e6 units from the eye, the program lights by
  the direction from the eye, a uniform branch: the Alatreon body's skinned
  three-light program 254.5 → 187.5 cycles, at most about a degree off for a
  vertex 2e4 units away [psp2cgc: same commit].
- **Blending** is part of the fragment program: the patcher builds one
  program per blend and output state (`PatchedProgram`), and consecutive
  draws with the same state reuse the last variant without a lookup [doc:
  material uniforms]. GX's subtract mode becomes REVERSE_SUBTRACT with both
  factors one; of the logic-op modes only COPY (a plain write) and NOOP (no
  colour write) are supported, and any other stops the game [code:
  `runtime/src/native_render_state.cpp`, `raster.cpp`].
- **Z compare before texture**: GXM always tests depth before shading. A GX
  material that writes depth "before texture" and also alpha tests would
  write depth for discarded pixels on the Wii; with `discard` on GXM the
  write only happens for survivors. The port does not emulate that case and
  stops with an error if it occurs (`RequireEarlyDepth`,
  `runtime/src/native_material.cpp`) [code].
- **Texture reads** use their varying unmodified, so the hardware issues
  them before the program runs (non-dependent reads) [doc: render thread;
  sdk].

Rules learned on hardware [doc: renderer status, "GXM rules"]:

- A float4 MODULATE equation compiled to a masked upper-lane multiply-add
  that returned zero blue and alpha on the device (the offline and runtime
  compilers produced the same code). Computing RG and BA as two float2
  chains fixed it.
- Matrix slot selectors are integral U8 attributes and are cast directly;
  `int(selector + 0.5)` made the optimized array index round slots 3 and 9
  up.
- Every attribute a vertex program reads must be bound (`0x805B0003`
  otherwise); attributes a mesh lacks read a constant (0,0,0,1) stream.
- Programs keep y: the viewport already flips it.
- **Keep vertex programs in parallel mode.** psp2cgc compiled three selects
  of the mesh vertex program (the normal's conditional normalize, the
  skinned matrix's singular-determinant fallback, the projective texgen
  divide) as dynamic flow control; such a program runs per instance, one
  vertex per USSE thread instead of four. Written as scalar selects and
  lerps they compile to parallel programs: the spot-lit vertex program 153
  cycles per instance → 146.75 parallel, the main scene's TA 3.9 → 3.5 ms
  per frame [psp2cgc, devkit: commit "Keep the Vita mesh vertex programs in
  parallel mode"]. A branch on a uniform keeps parallel mode.
- **Half precision**: colours in `half`, texture coordinates in `float`;
  11 significant bits against the target's 8, so results differ by at most
  one 8-bit step. Together with resolving some coefficients on the CPU it
  took Alatreon from 19.06 to 16.53 ms of GPU work per frame [devkit: shader
  specializations].

## 4. An offline shader bank

The Vita build embeds every program it can bind, compiled by Sony's
`psp2cgc` at build time ([shader bank](offline-shader-bank.md)):

- The program sources are a function of a small key; the generator
  enumerates the whole key domain: 4,180 vertex and 576 fragment stages of
  the fixed models, 35 stages of fixed GLSL programs (copy and palette
  conversion, depth peek, scene feedback) converted to Cg, and 8 stages of
  hand-written Cg utilities, 4,799 in all [code: the generator's count,
  2026-10-05]. The first bank, 2,648 programs, was 13.6 MB of GXP embedded
  as 1.01 MB with a zstd dictionary; the current one is 27.6 MB of GXP
  embedded as 2.2 MB [code: a development build's `shader_bank.json`,
  2026-10-05]. A program is decompressed the first time it is bound, and a
  quest binds a few dozen [doc: shader bank].
- `psp2cgc -O3 -fastmath -nofastprecision -fastint` under Wine in Docker;
  each GXP is cached by a hash of compiler, options and source. Stages that
  fail at `-O3` are built at `-O2` (ten skinned mesh vertex programs in the
  2026-10-05 bank); on arm64 build hosts `QEMU_CPU=core2duo` avoids crashes
  of the i386 compiler under qemu-user [doc: shader bank].
- Release builds have no runtime compiler: a miss stops the game.
  Development builds compile a miss with the on-device compiler
  (`libshacccg.suprx` through vitaShaRK) and report it. On the devkit that
  compiler cost seconds per program at first sight [doc: shader bank]; Sony's
  documentation reports tens of milliseconds to tens of seconds per program
  [sdk].
- `psp2shaderperf` gives cycle counts per program; the specializations
  were chosen with them, and the larger ones were confirmed with Razor
  captures on the devkit ([shader specializations](shader-specializations.md)).

## 5. Memory: CDRAM for everything the GPU reads

All GPU-read data lives in CDRAM (`SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW`,
mapped with `sceGxmMapMemory`): the VDM, vertex and fragment rings, the
shader patcher's buffers, the patched programs, the per-frame draw memory,
textures, render targets, converted models and material blocks [code:
`memory.cpp`, `device.cpp`]. Main RAM and phycont spill for textures is
budgeted at zero. Sony's documentation recommends CDRAM for textures, meshes
and render targets, an uncached main-memory ring for data the CPU rewrites
every frame, and never cached main memory for anything the GPU reads during
the frame [sdk]. This port puts its per-frame data (the frame slots'
vertices, indices and uniforms) in CDRAM as well; whether an uncached
main-memory ring would be faster for it has not been measured here.

The device splits the CDRAM left after libgxm's own allocations
[code: `device.cpp`, `native_capacity.hpp`]:

| User | Size |
| --- | --- |
| Frame slots | 3 × 3.5 MiB (1 MiB vertices, 2 MiB uniforms, 512 KiB U16 indices) |
| Auxiliary blocks | 3 × 256 KiB (vertex IDs, pixel-clip quad, utility quad) |
| Converted models | 16 MiB reserved (peak seen: 5.25 MiB) |
| Driver headroom | 8 MiB |
| Texture and display arena | the rest: 47 MiB on the devkit [devkit: commit "Give GXM textures 47 MiB of CDRAM by reserving 16 MiB for models"] |

Rules this port follows:

- **One arena for small allocations.** CDRAM blocks round to 256 KiB; the
  texture arena is one block with best-fit suballocation at 64-byte
  alignment, which GXM requires for texture bases [code: `memory.hpp`]. When
  it is full, the least recently used unbound textures are evicted
  [commit "Evict unbound textures when GXM's texture arena is full"].
- **Write CDRAM once, never read it back.** The CPU maps CDRAM uncached, so
  reading it back is slow. The mesh draw's 512-byte vertex block is built on the stack
  and copied once into the frame's uniform memory instead of being cleared
  there and filled field by field, and rigid draws leave the palette slots
  their shader does not read unwritten, about 1.3 KB fewer CDRAM writes per
  rigid draw [doc: render thread, "Per-draw work on the render thread"]. A
  material block that the CPU must resolve reads 144 bytes back and is kept
  rare [doc: material uniforms].
- **Persistent data is uploaded once.** Material blocks (784 bytes per
  classified structure) go into a 512 KiB CDRAM chunk at their first draw
  and are shared by every draw of that structure; only the 272-byte animated
  block is per draw, and it is reused when the material, texture and SRT
  revisions are unchanged [doc: material uniforms].
- **Textures are read straight into CDRAM** from the converted packs
  (`sceIoPread` into the allocation), with no decode or staging copy [doc:
  texture packs].

### Texture formats

GX formats are converted when the game loads a file
([asset conversion](../assets/load-time-asset-conversion.md)), into formats GXM samples
directly [code: `texture.cpp`; doc: texture packs]:

| Wii | GXM | Notes |
| --- | --- | --- |
| CMPR | `UBC1_ABGR`, or `UBC3_ABGR` when CMPR's transparent mode is used | block and palette formats are swizzled (Morton order), other sizes than powers of two through the arbitrary swizzle; [compressed textures](../assets/compressed-textures.md) |
| any other | the smallest exact one: `P4_ABGR`/`P8_ABGR` (palette in CDRAM), `U8_RRRR`/`U8_1RRR`/`U8_R000`/`U8_R111`, `U8U8_GRRR`, `U5U6U5_RGB`, else `U8U8U8U8_ABGR` | chosen from the decoded texels; palettes 64-byte aligned after the image |
| (lossy, explicit) | `UBC4_RRRR`, `UBC5_GRRR` | offline policy only |

Authored mipmaps are kept. The sampler's LOD bias is set in the texture
control word (`sceGxmTextureSetLodBias`) when it is exactly representable in
eighths between -3.875 and 4; anything else stops the game rather than
rounding a material's bias [code: `sampler_bias.hpp`]. A texture whose
image changes gets a new allocation while a scene still reads the old one,
which retires at its fragment notification, with no wait [doc: texture
packs].

## 6. Scenes on a tile-based GPU

- **Few scenes.** A frame has the main framebuffer's scene and one per
  feedback target (captures, glare passes). Render targets are created once
  per size and kept, with `scenesPerFrame` 8 [code: `device.cpp`]. The
  Alatreon fight has eight scenes per frame: the 3D scene (8.75-9.12 ms of 3D
  work), the scene capture, four glare passes, the 2D layers and the present
  (0.61 ms) [devkit: GPU capture, 2026-10-05].
- **One tap where filtering is exact.** A 2:1 box downsample that starts on
  a texel boundary averages each 2×2 block, which one bilinear sample at the
  block's shared corner does with the same weights: the glare's 160×112 and
  80×56 levels take one tap instead of four, 0.25 → 0.20 ms per frame
  [devkit: commit "Sample exact glare halvings once"]. The present samples
  texel centres when it copies at the source's size, where point sampling
  equals linear filtering: 0.61 → 0.58 ms; the pass is bound by memory
  traffic, not filtering [devkit: commit "Point-sample the present when it
  copies at the source's size"].
- **Do not load or store depth nobody reads.** A scene whose first operation
  is a full depth clear starts from the depth surface's background value
  with force-load disabled; the packet replay clears depth before colour so
  that the clear starts the scene. Main scene 3D 19.7 → 17.5 ms per frame,
  GPU busy median 29.6 → 27.4 ms [devkit: commit "Start the main scene from a
  cleared depth instead of loading it on the Vita"]. The replay also tells
  the renderer, per depth texture, whether anything after the scene reads
  depth; if nothing does, the store is skipped, and if a scene ever loads
  depth its predecessor did not store, the renderer logs once and stores
  everything from then on [code: `renderer.cpp`, `depth_scene`].
- **Keep hidden-surface removal on.** Colour masking prevents hidden-surface
  removal (Sony's libgxm overview, cited in [doc: shader specializations]).
  GX draws often write RGB and keep alpha. Every frame starts with a full
  clear to alpha 1, each render target carries a certificate that its
  stored alpha is all 1, and an unblended RGB-only draw to a certified
  target writes alpha 1 with a full mask instead. Desert quest GPU work
  20.89 → 18.53 ms per frame, main framebuffer 3D 13.44 → 11.07 ms [devkit:
  shader specializations].
- **Pixel-exact scissor** uses the mask bit: a mask-update fragment program
  draws a full-target quad with stencil function NEVER (clearing the mask)
  and the scissor rectangle with ALWAYS (setting it); region clip is left
  off [code: `raster.cpp`, `PixelClip::apply`]. The stored mask holds the
  previous scene's coverage, so the renderer redraws it at the start of every
  scene with depth or a scissor [code: `renderer.cpp`].
- **Render to texture without copies.** An EFB copy becomes a draw into a
  native render target, which later draws sample directly; nothing is read
  back to the CPU [doc: [scene feedback](scene-feedback.md)]. Passes that
  used the EFB as scratch space get simpler: the glare no longer draws its
  capture back, which saved 1.4 ms of 3D time per frame [devkit: commit
  "Skip the glare's draw-back of the scene it has just captured"].
- **No scene dependency flags.** No vertex program reads GPU-written data,
  so a scene's vertex work may overlap the previous scene's fragment work;
  Sony requires the dependency flags only when a fragment result feeds a
  later vertex job [doc: GX replacement].
- **Draw order inside a tile**: Sony's documentation recommends opaque, then
  discard, then blended draws [sdk]. This port keeps the game's own
  order and does not move discard draws, although the GPU guide describes a
  flush when discard is followed by opaque work in the same tiles.

## 7. Frame slots, sync objects and the display queue

Nothing in the frame loop waits for the GPU [code: `device.cpp`,
`gpu_completion.hpp`; doc: [presentation](presentation.md)]:

- **Notifications.** Every scene ends with `sceGxmEndScene` and a
  notification that writes the next serial into GXM's notification region.
  A slot or allocation is free when the region has passed its serial
  (`gpu_notification_pending`, a wrap-safe compare).
- **Three frame slots.** All per-frame vertices, indices and uniforms of a
  frame come from one 3.5 MiB slot; every scene that uses it extends its
  lease, and only a presented frame advances the ring.
- **Three display buffers**, each with a sync object passed to its scene
  and its `sceGxmDisplayQueueAddEntry`. `displayQueueMaxPendingCount` is 4
  for three admitted flips, because the callback counts a flip as done just
  before libgxm retires the entry.
- **Admission, not waiting.** Before preparing a packet the render thread
  checks whether the slot is still pending or three flips are queued; if so
  it skips the packet's draw (its resource tasks still run). Allowing three
  pending flips instead of two took the devkit's displayed rate in the
  Alatreon fight from 30.5 to 42.0 fps [devkit: triple buffering].
- **The display callback** (core 0) sets each buffer for the next VBlank and
  waits for it there, never on the render or game thread.
- **Retirement by notification.** Replaced textures, retired model packs
  and released material records are freed when their last scene's
  notification has passed. `sceGxmFinish` is not part of the frame loop: it
  runs at teardown, when a model pack's CDRAM allocation fails (to retire
  what the GPU still holds before retrying), and in readbacks
  [code: `renderer.cpp`, `texture.cpp`].

## 8. GX matrix palettes and skinning

GX's transform unit holds a small matrix memory; a shape's display list
loads up to ten position and normal matrices per batch and each vertex picks
one with an 8-bit index. NW4R models compute every skinning envelope on the
CPU and split shapes into batches of ten matrices.

On GXM a batch is one draw with a Palette uniform block (vertex
`BUFFER[2]`: twenty 3×4 position and texture matrices and ten normal
matrices) next to the Draw block (`BUFFER[0]`) and the Scene block
(`BUFFER[1]`) [code: `draw_memory.cpp`, `native_shader_source.cpp`]. That
reproduces GX exactly but multiplies draws: in the Alatreon fight about 260
model draws came from about 74 meshes, Alatreon's body mesh drew 101 times,
and the render thread spent about 70 µs per draw [devkit:
[gpu-skinning.md](gpu-skinning.md)].

Skinned meshes therefore draw once ([GPU skinning](gpu-skinning.md)): each
vertex carries four palette rows and weights from its envelope, the replay
writes the mesh's rows (rigid draw matrices, and bone view matrices times
inverse bind matrices) into a skinned Palette block of up to 255 rows, and
the vertex program blends three rows per influence. Devkit, Alatreon fight:
about 142 draws instead of 330, the render thread 14 ms per frame instead of
22.8 [devkit: commit "Draw skinned model meshes once, blending bones in the
vertex shader"]. The limits to check for another game: influences per
envelope (MH3: 2-4, 13 with more) and rows per mesh against the uniform
block.

## 9. The per-draw budget

On the devkit in the Alatreon fight the game thread was the limit (about
24.7 ms per frame in the baseline); the render thread took 17.5-19.4 ms per
packet before GPU skinning and about 14 ms after it, and the GPU 15.7 ms of
busy time per frame in the baseline devkit: [baseline,
commit "Draw skinned model meshes once, blending bones in the vertex
shader"]. The draw now overlaps the next logic step in every build
([pipelined draw](../threading/pipelined-draw.md)): 56.4 logical and 54.0 displayed fps
[devkit: commit "Split the world matrix pass with the guest worker in every
build"], with 15.76-16.47 ms of GPU work per frame [devkit: GPU capture]. The
render thread's time is mostly per-draw CPU work, which is what to cut:

- **Fewer draws**: cull on the CPU (the SGX culls after vertex shading
  [sdk]; the port's two box tests skip 28-34% of scene models and
  10-11% of mesh draws [devkit: renderer status]), merge adjacent same-state
  geometry (particles: 45-48% of submissions join the previous draw [arm64:
  effects port]), and skin on the GPU (section 8).
- **Fewer bytes per draw**: reference immutable material and pose versions
  instead of copying them ([state versions](../threading/state-versions.md)); write each
  uniform block once; skip identical vertex-array rebinds.
- **Fewer lookups per draw**: keep the last program, blend variant, model
  GPU record and frustum while their inputs repeat; no locks on the draw
  path; `FastMap` instead of `std::map` [doc: render thread, "Per-draw work on
  the render thread"].
- **Mind the Cortex-A9**: structs of 64 bytes or more are copied with
  `__aeabi_memcpy8`, there is no integer divide, and every `pthread_self`
  or mutex goes through TLS [doc: render thread].

## 10. Measuring

- **A GPU capture tool** (Sony's Razor): GPU job
  timing and counters per scene.
- **psp2shaderperf**: cycles and register counts per program; check every
  new variant.
- **A CPU profiler** on the Vita, flat or with stack snapshots
  [doc: [profiling](../../profiling.md)].
- **`MH3_PROFILE=render`** and `census`: the render thread's time by part of
  the replay, and the model draws per frame [doc: [flags](../../flags.md)].
- Compare CPU and GPU only from the same run; profiles of separate runs
  cannot show overlap.

## 11. Checklist for another Wii game

1. Replace GX at the draw producers, not at the FIFO: typed draws, a
   recorded frame, one render thread that owns libgxm.
2. Survey the game's materials on the disc; pick a few fixed programs that
   cover them; compile the whole key domain offline.
3. Convert textures and models when the game loads them; put textures,
   meshes and render targets in CDRAM (this port puts its per-frame data
   there too); never read CDRAM back.
4. Keep scenes few; skip depth loads and stores nobody reads; avoid colour
   masks that disable hidden-surface removal; scissor with the mask bit.
5. Turn EFB copies into render targets and CPU framebuffer reads into CPU
   work on the source data.
6. Fence everything with scene notifications; admit or skip frames instead
   of waiting; flip from the display callback.
7. Count draws per frame early; cull, merge and skin before micro-optimizing
   a draw.
