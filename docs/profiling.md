# Profiling

What to optimize: the game thread's milliseconds per frame on a Cortex-A9
(Vita core 2). A Raspberry Pi 5 (or any ARMv8 board) running the arm32
Cortex-A9 build shows relative CPU costs: it runs the game about 4-5 times
faster than a Vita, so aim for 4-5 times the speed wanted on the Vita
(240-300 game fps on the Pi 5 for 60 on the Vita). An x86 or arm64 desktop
with software rendering does not show these costs, and no desktop GPU says
anything about the Vita's. A good benchmark is the Alatreon fight (`--quest 19001 --area 1`)
with the frame limit off (`MH3_FRAME_LIMIT=off`); measure at least three
alternating runs per build.

## Statistics lines

Development builds (`MH3_DEVELOPMENT=ON`) print these about once a second
unless `MH3_STATS=0`. On the Vita they go to the console's TTY.

| Line | Meaning |
| --- | --- |
| `MH3 stats:` | game speed (paced retraces per second against the nominal VI rate), logical fps (completed game frames, also skipped ones), submitted fps, skipped fps, `logical_total`. With the variable step the game speed can stay near 100% at a low frame rate. |
| `MH3 native work: admitted render ... frame A avg/B max` | game frame bracket: frame begin through packet publication (input, scene update, logic, game-side draw preparation and recording); excludes render replay and the pacer, includes preemption |
| `MH3 native work: owner CPU` | the game thread's scheduled CPU over the window (Vita kernel `runClocks`, Linux thread CPU clock), with its thread ID, core and affinity |
| `MH3 native work: render replay ...` | completed packet replays per second and their mean wall time (resource tasks, replay, presentation); excludes waiting for the next packet |
| `MH3 native work: render waited for packets` | time the render thread waited for the game thread |
| `MH3 native work: render CPU` | the render thread's scheduled CPU, thread ID, core |
| `MH3 native work: displayed ... backlog ... callback wait` | completed display flips, queued display entries, pending GXM scenes, display errors and the display callback's wait |
| `MH3 native work: latency input to flip` | development, Vita: per flipped frame, the time from the frame's packet begin (just before the logic reads the input) to its flip, average and largest, and its parts: the game thread (to the packet's publication), the render thread's queue and replay (to the present), the GPU and earlier queued flips (to the display callback) and the VBlank wait |
| `MH3 frame split:` | the game frame timed as logic (input, scene update, tasks, timers) and draw (DrawScene, fonts, fade, HUD, movie) |
| `MH3 draw split:`, `MH3 scene split:` | the draw pass by family (traverse, model, effect, ..., the 2D layers and UI sprites around the scene) and DrawScene's setup, buckets and post effects |
| `MH3 packet snapshots:` | records per frame and state snapshots captured/stored per component |
| `MH3 quad runs:` | per drawn packet: runs of particle quads that differ only in their colours drawn as one, the quad records they drew, and runs refused ([particle batching](optimizations/rendering/particle-batching.md)) |
| `MH3 scene cull:`, `MH3 model cull:` | scene models culled on the game thread, mesh draws culled on the render thread |
| `MH3 native draw memory:`, `MH3 native CDRAM:`, `MH3 Vita free:` / `MH3 Vita budget:` | per-frame GPU memory peaks, texture/model residency, free memory partitions |
| `MH3 audio: native mixer` | the mixer's CPU time, ticks per second, output padded while the game is behind, active voices, thread and core |
| `MH3 material GPU:` | material-GPU counters every 300 frames (only when `MH3_STATS` is set) |
| `MH3 render thread: no progress ...` | the render thread made no progress for two reports: names the packet, record and kind |

Mean packet wall time is not CPU time; use the `CPU` lines for scheduled
execution and a profile for hotspots. `MH3_PROFILE=split` adds per-draw split
timers (`MH3 model split`): two clock reads per draw, and the `MH3 g3d split`
(scene processes by task), `MH3 field split` (the field update's calls) and
`MH3 layer split` (each 2D layer entry function, the HUD's parts) lines. The split timers read
`sceKernelGetProcessTimeWide` on the Vita (about 0.7 µs a read;
`sceKernelGetSystemTimeWide` 1.9 µs; the SDK's `gettimeofday` was a 6 µs
system call, which the port wraps).

### Where the A9's cycles go

Measured with the Cortex-A9's performance monitor (libperf) in the Alatreon
fight with the frame limit off: the game thread runs at IPC 0.42 with 40
I-cache misses and 18 branch mispredicts per 1000 instructions, 15% of its
cycles stalled on the I-cache and 16% on the D-cache; the render thread at
IPC 0.43 with 68 I-cache misses and 37 mispredicts per 1000 instructions,
10% and 25% stalled. Large translated code is I-cache bound: PGO and
smaller hot paths help more than arithmetic tuning.

## Vita CPU profile

Sample the game on the Vita with a PC-sampling profiler and symbolize
against the unstripped ELF (`build/vita/MH3`). Development builds keep frame
pointers (`MH3_PROFILE_FRAME_POINTERS`, default: the value of
`MH3_DEVELOPMENT`: `-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
-funwind-tables`, ARM32 also `-mframe-chain=aapcs+leaf`) for the game, the
runtime and Aurora. Compare timings only between builds with the same
setting. A release build has no unwind tables and no exceptions: profile
development builds.

Rules for reading such profiles:

- Percentages are of the thread's user-PC samples; count kernel-only
  samples separately.
- Stack walks from a timer interrupt are fragile on the Vita: deep user
  stack backtraces or large stack snapshots overrun the sampler's stack and
  can freeze the console. Keep backtraces off or shallow, and stack
  snapshots small (a few KiB).
- Compare CPU and GPU only from the same run.

### Memory and math helpers in Vita profiles

The EABI memory helpers (`__aeabi_memcpy`, `memmove`, `memset`, `memclr` and
their 4/8 forms) come from `runtime/vita/aeabi_memory.c`, a C port of the
compiler-builtins routines (16-byte NEON loops for the aligned copy and set;
[helpers](rom-decoder.md#vita-runtime-helpers)).
Public `memcpy`, `memmove` and `memset` tail-call
`sceClibMemcpy`/`Memmove`/`Memset`; `memcmp` is the SDK's scalar loop. The
aligned copy and set helpers clobber `d16`/`d17`, which the Arm runtime ABI
reserves for the caller (a helper/ABI mismatch kept from compiler-builtins,
not a known fault). Hot byte comparisons in the renderer
use `BytesEqual` (`runtime/include/bytes_equal.h`, NEON, equality only)
instead of `memcmp`. The SDK's TLS lookups for its prebuilt libraries go
through the fast paths in [Vita TLS lookups](optimizations/platforms/vita-tls-lookups.md).

### Barriers on ARM32

On ARMv7 an acquire load or a sequentially consistent atomic is `ldr` plus
`dmb ish`, and so is the guard test of every function-local `static` with a
dynamic initializer (`static const bool check = f();`), on every pass. A
`std::shared_ptr` copy or release is an atomic read-modify-write between two
`dmb`s. The Pi 5's Cortex-A76 hides most of their cost, so Pi timings barely
move when barriers go; the Vita's Cortex-A9 waits at each one for earlier
memory accesses. Judge barrier work by counts. The per-function census of a
build:

```sh
arm-linux-gnueabihf-objdump -d --no-show-raw-insn build/linux-arm32/MH3 \
  | awk '/^[0-9a-f]+ </{f=$2} /\tdmb\t/{n[f]++} END{for(f in n) print n[f], f}' | sort -rn
```

Intersect it with the function list of a Vita CPU profile to find the hot
ones. Keep flags in namespace-scope variables or relaxed atomics (as
`PortCheck::Enabled` does), and keep guarded statics and `shared_ptr` copies
out of per-call paths.

## ARM boards and desktops

On an ARM Linux board, `perf record` of the arm32 build gives flat profiles;
its ARM32 call stacks are unreliable. For call graphs, profile the arm64
build with `perf record --call-graph fp`, or record
`--call-graph dwarf,32768` and unwind offline with libdwfl against the exact
ELF files.

## Stalls

Development builds report single slow frames, which the per-second averages
hide:

- `MH3 long frame N: X ms (logic, draw, other; pacing wait, draw thread
  handoff); most: ...`: a game frame over 40 ms, with its waits for the render
  thread (pacing) and the draw thread (handoff) and the split families that
  took the most (`game_frame.cpp`; the draw thread's families count too).
- `MH3 long NAME job: X ms`: a guest worker or draw thread job over 40 ms
  (`guest_worker.cpp`).
- `MH3 long packet N: X ms: GPU slot wait, before tasks (count, slowest, from
  ADDRESS), draw, after tasks`: a render-thread packet over 40 ms
  (`native_frame_packet.cpp`); symbolize the task's address against the
  build's ELF (`addr2line`).
- `MH3 slow shade program`, `MH3 GXM slow ...`, `MH3 slow scene setup call`:
  shader program creation over 2 ms and scene setup calls over 20 ms.
