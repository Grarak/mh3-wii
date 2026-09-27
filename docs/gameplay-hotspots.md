# Current-build gameplay hotspot breakdown — 2026-09-27

> Platform note: results in this document were recorded on the earlier macOS
> ARM64/Metal build. The port now targets arm64 Linux with Vulkan; see
> [arm64 Linux and Vulkan](linux-arm64.md) for what has been re-verified.

Profiled release commit f44f156, -O1, all diagnostic flags OFF, Metal renderer,
post-Chief PAL save and 4:3 presentation. No Dolphin run or concurrent build.
Two ten-second macOS `sample` captures: 5,239 main-thread samples standing at
Moga Woods entrance and 5,206 while alternating the left stick horizontally
at ±350 every 0.5 seconds. This is a local movement test, not combat or every
area. Tutorial dialogs were dismissed first. Periodic BMP capture remained at
one per 30 rendered frames, matching prior benchmarks; these measurements
include that capture overhead. Sampling itself also has overhead.

Raw evidence: `logs/hotspots-field-idle.txt`, `logs/hotspots-field-moving.txt`,
frame counters/timings in `logs/replays/hotspots/mh3.log`; JSON summaries adjacent
to the raw samples. The one-second startup warmup is excluded.

Percentages below are **main-thread sampled wall time**, including waits.
Self excludes callees; inclusive includes them. Nested inclusive rows overlap
and must not be added. Recursion is deduplicated per stack by
`scripts/summarize_cpu_sample.py`; direct-child subtraction accounts for every
sample. Checked against a synthetic recursive tree and both live profiles.

| Function/path | Idle inclusive | Moving inclusive | Moving self |
|---|---:|---:|---:|
| Guest GX processing (`GuestGxStream::Write`) | 70.89% | 72.24% | 1.52% |
| Aurora FIFO processing (`aurora::gx::fifo::process`) | 40.81% | 40.95% | 1.00% |
| Draw submission (`aurora::gx::fifo::handle_draw_unmerged`) | 27.05% | 27.58% | 0.25% |
| Graphics buffer append/copy (`aurora::gfx::push`) | 20.25% | 20.65% | 1.42% |
| Memory copying (`_platform_memmove`) | 19.66% | 19.61% | 19.61% |
| EFB copy to RAM (`aurora_flush_efb_copy_to_ram`) | 10.57% | 10.45% | 0.00% |
| Pipeline-state lookup (`aurora::gx::fifo::resolve_pipeline_state`) | 8.02% | 7.93% | 1.94% |
| Vertex-array index scan/bind (`GuestGxStream::BindDrawArrays`) | 5.29% | 5.28% | 2.96% |
| Uniform construction (`aurora::gx::build_uniform`) | 2.29% | 2.75% | 2.15% |
| Texture packet construction (`GuestGxStream::TexturePacket`) | 2.58% | 2.92% | 0.02% |
| Graphics completion (`CompleteHostGraphics()`) | 3.26% | 3.52% | 0.00% |
| Audio filter 80475880 (`func_80475880`) | 3.99% | 3.88% | 2.17% |

XXH3 hashing functions together account for 6.28% **self** time during movement;
`memcmp` is another 2.21%. Both are already included within the parent paths
above. Condition-variable waits are 11.37% self and `__semwait_signal` is 4.90%;
they include synchronization/pacing, not guest arithmetic or exclusively GPU
execution. Process frame logging remains around 42 ms CPU and 50 ms wall per
rendered frame, ~13,100 draws, ~45,000 array bindings, five EFB texture copies
(~4.9 ms), one EFB snapshot (~0.8 ms), and 8,960 EFB peeks. Graphics completion
wait is ~1.7 ms (~0.4 worker / ~1.3 GPU queue). These are host waits rather than
GPU timestamp durations, so they do not independently measure total GPU load.

## Decompiled functions

| Address / current label | Moving inclusive | Moving self |
|---|---:|---:|
| `80076A58` RenderDisplayListDispatch | 58.61% | 0.04% |
| `80087B34` SetupAndSubmitDisplayList | 54.30% | 0.02% |
| `8009A0DC` SubmitResolvedDisplayList | 54.15% | 0.00% |
| `80475880` AudioThreeChannelFilterCallback | 3.88% | 2.17% |

These descriptive labels are already retained in `symbols.txt`. The rendering
chain is 80076A58 → 80087B34 → 8009A0DC → FIFO/display-list processing → Aurora.
It looks expensive by inclusive cost because it invokes the host renderer.
Direct samples symbolized to all generated `func_*` bodies total only 7.28%
(inlined runtime operations may also be attributed to those bodies). The audio
filter is the only generated function above 1% self time; its inclusive share
fell from 21.54% in the old profile to 3.88% now, largely because the runtime RAM
access bottleneck was removed. No collision function is a leading self hotspot.

## Optimization priority

1. **Avoid redundant buffer uploads/copies.** `gfx::push` consumes 20.65%
   inclusive; call stacks show large memcpy costs under `handle_draw_unmerged`.
   Aurora uploads each indexed array when `cachedRange` is empty. The bridge
   binds a full RAM view on CP array-base writes and then trims to the draw's
   maximum index; pointer/size changes invalidate Aurora's cached upload.
   Measure uploaded bytes and invalidation reasons next, then reuse ranges
   within a safely bounded lifetime. Do not assume guest arrays are immutable.
2. **Reduce pipeline/texture hashing and array scans.** Pipeline lookup is
   7.93%, array scanning/binding 5.28%, all XXH3 hashing 6.28% self (overlapping
   with those parents). Track state changes instead of repeating identical work
   where correctness permits; cache parsed display-list/index metadata with
   explicit invalidation.
3. **Reduce synchronous EFB readbacks.** RAM flush is 10.45% inclusive, including
   waits; it is still a meaningful target despite the small final-frame fence.
   A deferred copy must preserve guest CPU reads. Disabling synchronization
   wholesale would not be a verified optimization.
4. **Only then rewrite the audio filter.** 80475880 has 2.17% self / 3.88%
   inclusive, far smaller than the bridge. Preserve PPC rounding and compare
   buffers against the existing implementation before replacing it.

The current evidence points primarily to CPU graphics-bridge overhead plus
readback synchronization. Moving and stationary percentages agree closely;
this test does not support collision math as the explanation for low FPS.
No runtime behavior was changed in this profiling pass.

Reproduce summaries:

```sh
python3 scripts/summarize_cpu_sample.py logs/hotspots-field-moving.txt
python3 scripts/summarize_cpu_sample.py logs/hotspots-field-moving.txt --json
```
