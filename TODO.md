# Follow-ups

Target: arm64 Linux with Vulkan. Items marked done were verified on the earlier
macOS/Metal build; see `docs/linux-arm64.md` for what has been checked on Linux.

## arm64 Linux bring-up

- [x] Vulkan-only renderer request (fails on backend fallback), arm64 Linux
  build system with cross toolchain, Linux-aware scripts and docs.
- [x] Cross-compile check: Aurora, SDL3, MH3 overlays and runtime sources build
  for aarch64 against the Vulkan Dawn package.
- [ ] Translate on Linux, link `build/linux-arm64/MH3`, pass `--self-test`,
  `--graphics-check`, `--efb-check` and CTest on arm64 hardware.
- [ ] Re-verify boot, village, Moga Woods, input and audible audio on Vulkan.
- [ ] Record the target GPU's Vulkan memory types; validate and benchmark
  `MH3_SHARED_RENDER_BUFFERS=1` against the copy path.
- [ ] Linux profiling workflow (`perf`) to replace macOS `sample` tooling.

## Optimization and gameplay

- [ ] Profile and reduce the slow village → Moga Woods loading/transition.
  Measure exit selection → first field frame → player control, separately
  timing disc reads, decompression, RSO load/relocation/checksums, asset setup,
  texture upload and shader/pipeline creation. Compare cold and warm runs
  with Dolphin using the same post-Chief save and 4:3 presentation.
  Reproduce with `scripts/replay_village.py --name NAME --moga-woods`.
- [x] Initial CPU profile and RAM-access optimization after the collision fix:
  field CPU ~57→42 ms, displayed frame interval ~67→50 ms. See
  `docs/rendering-performance.md` for measurements and function labels.
- [ ] Further improve frame pacing/full-speed gameplay. Profile repeated Aurora
  array/uniform uploads, then the labelled audio filter before a reference-checked
  readable rewrite. Keep -O1 unless measured gains justify changing it.
  Current idle/movement breakdown and ranked targets: `docs/gameplay-hotspots.md`.
- [ ] Verify village building collisions later (dock boundary regression passes).
- [x] Verify Dolphin's background savestate/screenshot pipe.
- [ ] Extend fast state replay to reusable field checkpoints.
- [x] Fix dark top/left edges in village and Moga Woods. Aurora decoded GX
  viewport origins with bias 340 instead of 342; fixed both decoder and native
  encoder in a local source overlay. See `docs/rendering-performance.md`.

- [x] Enable bounded asynchronous completed-frame encoding while keeping
  SDL on the main thread (validated on macOS; now also applied on Linux). See `docs/async-rendering.md`.
- [ ] Move expensive GX decode/array preparation off-thread with explicit guest
  resource ownership; the new frame worker does not move that work yet.
