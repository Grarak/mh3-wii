# Profiling

What to optimize: the game thread's milliseconds per frame on a Cortex-A9
(Vita core 2). The Pi 5 runs the same Cortex-A9 build for CPU comparisons;
the VM's llvmpipe timings are not representative, and the Pi's GPU says
nothing about the Vita's. The benchmark is Alatreon
(`--quest 19001 --area 1`). Measure on the Pi with at least three alternating
runs per build.

GPU captures on the Vita (Sony Razor) are in [vita-razor.md](vita-razor.md).

## Statistics lines

Development builds (`MH3_DEVELOPMENT=ON`) print these about once a second
unless `MH3_STATS=0`. On the Vita they go to the TTY, which the devkit
harness records as `[TTY0]` lines in `RUN/console.txt`.

| Line | Meaning |
| --- | --- |
| `MH3 stats:` | game speed (paced retraces per second against the nominal VI rate), logical fps (completed game frames, also skipped ones), submitted fps, skipped fps, `logical_total`. With the variable step the game speed can stay near 100% at a low frame rate. |
| `MH3 native work: admitted render ... frame A avg/B max` | game frame bracket: frame begin through packet publication (input, scene update, logic, game-side draw preparation and recording); excludes render replay and the pacer, includes preemption |
| `MH3 native work: owner CPU` | the game thread's scheduled CPU over the window (Vita kernel `runClocks`, Linux thread CPU clock), with its thread ID, core and affinity |
| `MH3 native work: render replay ...` | completed packet replays per second and their mean wall time (resource tasks, replay, presentation); excludes waiting for the next packet |
| `MH3 native work: render waited for packets` | time the render thread waited for the game thread |
| `MH3 native work: render CPU` | the render thread's scheduled CPU, thread ID, core |
| `MH3 native work: displayed ... backlog ... callback wait` | completed display flips, queued display entries, pending GXM scenes, display errors and the core-0 display callback's wait |
| `MH3 frame split:` | the game frame timed as logic (input, scene update, tasks, timers) and draw (DrawScene, fonts, fade, HUD, movie) |
| `MH3 draw split:`, `MH3 scene split:` | the draw pass by family (traverse, model, effect, ...) and DrawScene's setup, buckets and post effects |
| `MH3 packet snapshots:` | records per frame and state snapshots captured/stored per component |
| `MH3 scene cull:`, `MH3 model cull:` | scene models culled on the game thread, mesh draws culled on the render thread |
| `MH3 native draw memory:`, `MH3 native CDRAM:`, `MH3 Vita free:` / `MH3 Vita budget:` | per-frame GPU memory peaks, texture/model residency, free memory partitions |
| `MH3 audio: native mixer` | the mixer's CPU time, ticks per second, output padded while the game is behind, active voices, thread and core |
| `MH3 material GPU:` | material-GPU counters every 300 frames (only when `MH3_STATS` is set) |
| `MH3 render thread: no progress ...` | the render thread made no progress for two reports: names the packet, record and kind |

Mean packet wall time is not CPU time; use the `CPU` lines for scheduled
execution and a profile for hotspots. `MH3_PROFILE=split` adds per-draw split
timers (`MH3 model split`): two clock reads per draw. The split timers read
`sceKernelGetProcessTimeWide` on the Vita. Avoid `steady_clock` in hot code
there: it is a `gettimeofday` system call. The startup line
`MH3 Vita clock read cost` reports the clocks: `steady_clock` about 6 µs,
`sceKernelGetProcessTimeWide` about 0.7 µs (`Low` 0.3 µs),
`sceKernelGetSystemTimeWide` 1.9 µs.

## Vita CPU profile

Only the main session uses the devkit; agents in worktrees use the VM and the
Pi (the harness refuses to run from `.claude/worktrees`). Every devkit run
goes through `scripts/dev/devkit-heartbeat.sh` and is profiled (see
[HANDOVER.md](../HANDOVER.md#vita-devkit)).

Stage `RUN/game.elf` (`build/vita/MH3`) and `RUN/game.velf`
(`build/vita/MH3.velf`) next to `RUN/host0/game/`; the harness checks the
pair's unwind registration (`scripts/vita-check-unwind.py`) before it starts.
`--cpu-profile-seconds N` (1–60) starts the collector after the readiness
count (`--ready-copies`, default 300 frames):

- **Flat** (no `--cpu-registers`): Sony's DTrace sampler at 997 Hz, PC only.
  Use this in Alatreon.
- **With stack snapshots** (`--cpu-registers --cpu-stack-bytes N`): a second
  stream of registers and N bytes of stack at 199 Hz for offline caller
  recovery. Snapshots can freeze the Vita: in Alatreon, 8 KiB snapshots froze
  it in about half the runs and 32 KiB within about 4 s of sampling; during
  the THP intro they stopped the process's output. The render thread's
  snapshots all fail, so it has no stacks.

Outputs: `native-cpu.samples`, `.txt`, `.vprof`; with snapshots also
`native-cpu.register-info.json`, `.registers.jsonl`, `.stack.bin`. The
wrapper then writes `RUN/profile-summary.txt` with
`scripts/vita-profile-summary.py RUN`: per thread (game, render, audio mixer,
from the `CPU` lines and the mixer's startup line) the available samples, the
hottest functions and the shares of TLS, copy/clear and division helpers.

Further analysis with psp2-lle's vprof CLI
(`/path/to/psp2-lle/build/vprof-ui/vprof-cli`):

```sh
vprof-cli threads RUN/native-cpu.vprof --elf RUN/game.elf
vprof-cli functions RUN/native-cpu.vprof --elf RUN/game.elf --thread thread-0xTID --top 30
vprof-cli callers RUN/native-cpu.vprof SYMBOL --elf RUN/game.elf
```

Rules for reading them:

- Percentages are of the thread's available user-PC samples. Rows with only a
  kernel PC are kernel time; count them in `native-cpu.samples`. Missing PCs
  (zero or `0xffffffff`) are kept as unavailable rows.
- DTrace timestamps have their own origin and rate; do not derive utilization
  from the requested duration or merge them with Razor timelines.
- Sony's USTACK action returns no callers from the profile timer: the
  firmware's backtrace service rejects the running interrupted thread
  (`SCE_KERNEL_ERROR_RUNNING`). Callers come only from the snapshot stream.

### Callers from snapshots

```sh
python3 scripts/vita-unwind-snapshots.py --info RUN/native-cpu.register-info.json \
  --elf RUN/game.elf --out RUN/analysis/snapshot-cfi --max-depth 64 --rust-memory-cfi
python3 scripts/vita_vprof.py --samples RUN/analysis/snapshot-cfi.samples \
  --stacks RUN/analysis/snapshot-cfi.stacks.jsonl --stack-source 'SHA-verified ARM32 DWARF CFI' \
  --deci-log RUN/deci4p.log --elf RUN/game.elf --hz 199 --out RUN/analysis/snapshot-cfi
vprof-cli functions RUN/analysis/snapshot-cfi.vprof --elf RUN/game.elf --inclusive --top 30
```

The unwinder checks the recorded hashes, the exact ELF and the interrupted
context, reads only the captured stack bytes and uses DWARF CFI; it never
scans for return addresses. `--rust-memory-cfi` adds audited frame rules for
the Rust memory helpers of `MH3_ROM_DECODER_NOD` builds
([details](vita-rust-async-cfi.md)); other builds' C helpers have their own
CFI, and the option is skipped. Every chain is a
prefix that stops at the captured stack's end, a missing CFI frame or an
unverified mapping; 8 KiB stacks favour shallow draw-phase stacks. Snapshot
events are a separate stream: never pair them with primary samples or use
their counts as CPU-time weights. Leaf helpers without CFI (`__udivmodsi4`,
the `gettimeofday` stub) can be attributed through the snapshot's link
register.

### Build configuration

`MH3_PROFILE_FRAME_POINTERS` (default: the value of `MH3_DEVELOPMENT`) adds
`-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -funwind-tables` (ARM32
also `-mframe-chain=aapcs+leaf`) to the game, the runtime and Aurora, and
frame pointers and unwind tables to the Rust ROM decoder. Compare timings only
between builds with the same setting.

`scripts/build-vita.sh` packages with the patched `vita-elf-create`
(`/path/to/vita-toolchain/build-unwind/src/vita-elf-create`, or
`MH3_VITA_ELF_CREATE`), which registers the real `.ARM.exidx`/`.ARM.extab`
ranges in the module info (the stock tool writes zero ranges) and emits the
module TLS template. It then runs `scripts/vita-check-unwind.py`, which fails
the build for missing or wrong registration and writes
`build/vita/unwind-packaging.json`.

### Memory and math helpers in Vita profiles

From audits of the October 2026 ELFs: `__aeabi_memcpy4/8`, `__aeabi_memclr*`
and the generic `compiler_builtins::mem::memcpy`/`memmove` come from the Rust
ROM decoder's `compiler_builtins` (16-byte NEON loops for the aligned
variants), and are linked ahead of the SDK. Public `memcpy`, `memmove` and
`memset` tail-call `sceClibMemcpy`/`Memmove`/`Memset`; `memcmp` is the SDK's
scalar loop. The aligned `compiler_builtins` copy and set helpers clobber
`d16`/`d17`, which the Arm runtime ABI reserves for the caller (a
helper/ABI mismatch, not a known fault). Hot byte comparisons in the renderer
use `BytesEqual` (`runtime/include/bytes_equal.h`, NEON, equality only)
instead of `memcmp`. The SDK's TLS lookups for its prebuilt libraries go
through the fast paths in [vita-tls-lookup-optimization.md](vita-tls-lookup-optimization.md).

## Pi and VM

`scripts/pi/benchmark.py` (copied to `~/mh3` on the Pi) runs a quest with an
isolated save and scripted movement and reports idle and movement game fps
and scheduled CPU ms per frame; see [HANDOVER.md](../HANDOVER.md#run).

The Pi's `perf report` gives no usable ARM32 call stacks. For call graphs,
profile the arm64 build on the VM with `perf record --call-graph fp`
(`perf_event_paranoid` needs root, so run perf through Docker).

`scripts/pi/perf_arm32_stacks.py` recovers callers offline from a Pi
`perf record --call-graph dwarf,32768` capture: it reads the recorded
registers and stack bytes with libdwfl and the exact ELF files (mirror the
Pi's libraries with `--symfs`). It computes the relocation bias from the
mapping's file offset and the executable PT_LOAD (perf's own frontend picks a
bias 64 KiB off for this executable layout and fails). The Pi's libc and libm
have no DWARF frame data, so their memcpy/memcmp/memset/fmaf samples stay
without callers; in the October 2 captures about 45% of samples recovered at
least one caller.
