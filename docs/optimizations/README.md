# What this port changes beyond a static recompilation

A straight static recompilation translates a game's PowerPC code to C++ and
runs it on an emulation of the Wii's hardware and system software. That is
far too slow for a PS Vita: its four Cortex-A9 cores at 444 MHz and its
SGX543 GPU have to do the work of the Wii's CPU, GPU, DSP and IOS
processor. This port replaces whole subsystems with native code and changes
how the game runs, until the game thread, the render thread and the GPU
each fit a frame.

Each page below describes one change in the same parts: what the original
game does (with addresses), what the port does instead, why (with the
measurements taken in this project), an example from the code, and how to
apply it to another Wii game. Measurements name their machine: a PS Vita
development kit (the Vita's CPU and GPU; "Vita" in the pages), a Raspberry
Pi 5 running the Cortex-A9 build ("Pi 5"), or an arm64 Linux build with
software rendering ("arm64"). The Pi 5 shows relative costs, not Vita
speed: it runs the game about 4-5 times faster than a Vita, so a build is
on track when the Pi 5 reaches 4-5 times the speed wanted on the Vita
(240-300 game fps for 60 on the Vita).

## Porting another Wii game: an order that works

1. **Recompile and boot** on a desktop build: discover every function and
   jump target, translate the relocatable modules, and replace the system
   layer (IOS, OS, devices). Make the GX SDK fatal so that every draw path
   shows up.
2. **Replace GX** family by family with native draw producers, recorded
   into frame packets for a render thread. Classify the game's materials
   into a few fixed shading programs.
3. **Convert assets** when the game loads them: textures into native
   formats, models into interleaved meshes with GPU skinning.
4. **Bring up the Vita** platform layer and the libgxm backend; compile every
   shader ahead of time.
5. **Profile on the Vita** (or an ARMv7 board first) and port the hot
   translated functions to native C, leaves first. Move work off the game
   thread: the draw, effects, model preparation, audio, disc reads.
6. **Cut GPU time** with specializations, culling and fewer draws.
7. **Build releases** with ThinLTO and a profile collected over every scene.

## Recompilation

| Page | Replaces | Why |
| --- | --- | --- |
| [Function discovery](recompilation/function-discovery.md) | finding code one crash at a time | every indirect target known at build time |
| [Static RSO overlays](recompilation/static-rso-overlays.md) | loading untranslated module code at run time | no interpreter or JIT exists |
| [Translator call boundary](recompilation/translator-call-boundary.md) | complete-context fences and guards at calls | call overhead (Alatreon game thread 8.27 → 7.83 ms for direct precise natives alone) |
| [Flag elision](recompilation/flag-elision.md) | every CR field and the carry computed up to each call and return | Vita Jhen Mohran logic 12.48 → 11.48 ms a frame (with the stfs fold) |
| [Cortex-A9 floating point](recompilation/cortex-a9-floating-point.md) | exact fused multiply-add on arm32 | about 0.56 ms a frame on the Pi 5; no visible difference |

## CPU

| Page | Replaces | Why |
| --- | --- | --- |
| [Native function ports](cpu/native-function-ports.md) | translated bodies of hot or step-dependent functions (about 1,650) | the translated code's register and memory model costs time; some behaviour has to change |
| [Flat guest memory](cpu/flat-guest-memory.md) | a software MMU lookup on every load and store | every access paid for it; allows a second guest thread |
| [Compact native data](cpu/compact-native-data.md) | walking guest collision data per query | ground queries 1.3 → 0.12 ms a frame (Vita) |
| [Barriers on ARM32](cpu/memory-barriers.md) | acquire loads and guarded statics on per-call paths | translated code's `dmb`s 7,431 → 2,988; the A9 waits at each |
| [PGO and ThinLTO](cpu/pgo-and-thinlto.md) | a plain optimized build | +13% logical fps (Vita, Alatreon) |

## Threading

| Page | Replaces | Why |
| --- | --- | --- |
| [Render thread and frame packets](threading/render-thread.md) | drawing and presenting on the game thread | Pi 5 Alatreon 49 → 145 game fps |
| [Pipelined draw](threading/pipelined-draw.md) | logic and draw back to back on one thread | Vita Alatreon logical fps 36.2 → 42.9 when first overlapped |
| [Guest worker](threading/guest-worker.md) | the effect calculation inside the task pass; half the world matrix pass | 4.5-7.4 ms a frame of effects off the game thread |
| [State versions](threading/state-versions.md) | per-draw copies of poses and material fields | copies were 7.6% of the Vita game thread |
| [Core layout](threading/core-layout.md) | one CPU core | the game thread's core for it alone; the system's core for workers only |

## Rendering

| Page | Replaces | Why |
| --- | --- | --- |
| [libgxm for Wii ports](rendering/libgxm-for-wii-ports.md) | (a guide) | how each GX concept maps onto GXM |
| [GX replacement](rendering/gx-replacement.md) | the GX FIFO, TEV/XF/BP state, display lists | GX emulation cost; a retail Vita cannot compile generated shaders |
| [Fixed shading models](rendering/fixed-shading-models.md) | TEV combiner programs | 92 topologies, three cover 92.5%; 10 instead of 46-175 cycles a fragment |
| [Material uniform blocks](rendering/material-uniform-blocks.md) | per-draw CPU resolution of material constants | render thread 26.4 → 24.4 ms a packet (Vita) |
| [Offline shader bank](rendering/offline-shader-bank.md) | runtime shader compilation | no compiler on retail units |
| [Shader specializations](rendering/shader-specializations.md) | the general programs for every draw; per-pixel fog | Vita GPU work per frame down by 2-2.5 ms per step |
| [Deferred model preparation](rendering/deferred-model-preparation.md) | g3d's material, primitive and matrix setup on the game thread | Vita model drawing 20.6 → 12.7 ms, then 5.7 → 4.5 ms a frame |
| [Compiled dispatch materials](rendering/compiled-dispatch-materials.md) | re-merging material state per dispatch | Vita village model preparation 4.78 → 3.19 ms a packet |
| [Precomputed draws](rendering/precomputed-draws.md) | per-draw GXM state setup and full uniform blocks | render thread 15.30 → 14.51 ms a packet (Vita) |
| [GPU skinning](rendering/gpu-skinning.md) | palette batches of ten matrices and CPU envelopes | about 142 draws instead of 330, render thread 22.8 → 14 ms (Vita) |
| [Culling](rendering/culling.md) | the game's depth-only visibility test | 28-34% of scene models skipped (Vita) |
| [Scene feedback](rendering/scene-feedback.md) | EFB copies to RAM, CPU EFB access | no readback stalls; 1.4 ms of 3D a frame |
| [Particle batching](rendering/particle-batching.md) | per-particle immediate GX draws | Pi 5 game thread 4.29-4.58 → 4.04-4.08 ms |
| [HUD and debug font](rendering/hud-and-text.md) | per-shape double binds, per-character glyph table walks | Vita notice screen 13.00 → 5.72 ms a frame; HUD layers 2.05 → 1.82 ms |
| [Presentation](rendering/presentation.md) | VI and XFB, CPU waits for the GPU | displayed 30.5 → 42.0 fps with three pending flips (Vita) |
| [Widescreen](rendering/widescreen.md) | an anamorphic 640×448 picture | the Vita's whole display at 960×544 |

## Assets

| Page | Replaces | Why |
| --- | --- | --- |
| [Disc access](assets/disc-access.md) | a disc drive or an extracted partition | first start 8 min 42 s → 0.25 s (Pi 5); loads never block the game thread |
| [Load-time asset conversion](assets/load-time-asset-conversion.md) | GX textures and display lists read from RAM | no decoding while drawing; 0.35 s of conversion CPU instead of 18 s for the whole disc |
| [Compressed textures](assets/compressed-textures.md) | CMPR decoded to 32-bit texels | BC1/BC3 blocks rewritten losslessly: an eighth (BC3: a quarter) of RGBA8's bytes; 210 instead of ~1,040 MiB of textures |
| [Hardware movie decoding](assets/hardware-movie-decoding.md) | software THP decoding on the game thread | movies at 60 instead of 30 fps (Vita) |

## System

| Page | Replaces | Why |
| --- | --- | --- |
| [System layer](system/system-layer.md) | IOS, interrupts, the decrementer, VI hardware | translated code cannot be interrupted |
| [Saves on host files](system/saves-nand.md) | the NAND file system | the game's own save code runs unchanged |
| [Native audio](system/native-audio.md) | AX, the DSP and the translated sound engine | Vita desert quest 14.4-14.9 → 20.3 fps |
| [Controller input](system/controller-input.md) | Wii Remote with Classic Controller | the hosts have neither |

## Simulation

| Page | Replaces | Why |
| --- | --- | --- |
| [Variable step](simulation/variable-step.md) | fixed 1/30 s steps | the frame rate is set by the hardware, not by 30 Hz |

## Platforms

| Page | Replaces | Why |
| --- | --- | --- |
| [Vita platform layer](platforms/vita-platform-layer.md) | the Wii hardware | Vita SDK limits (clock calls, semaphores, stdio) |
| [Vita TLS lookups](platforms/vita-tls-lookups.md) | the SDK's emulated TLS chain | TLS helpers down to 0.2% of the game thread |
| [Vita CDRAM budget](platforms/vita-cdram-budget.md) | the Wii's shared memory | all GPU data in video memory, never read back |
| [Linux: GLES 3.0 and the Cortex-A9 build](platforms/linux-gles-and-arm32.md) | Aurora's WebGPU/Dawn renderer | test the Vita's CPU model and renderer structure on a Pi or another ARM Linux machine |
| [Web build](platforms/web-build.md) | the Wii hardware, in a browser | the same game in a browser; not a performance target |
