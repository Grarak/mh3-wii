# MH3 handover — 2026-09-27

## Start here

**Target platform: arm64 (aarch64) Linux, Vulkan only.** This is a static PowerPC → C++ recompilation of Monster Hunter Tri. Graphics go through Aurora on Dawn's Vulkan backend; it is not a Dolphin frontend. There is no interpreter/JIT fallback. The active translated game is **European RMHP08**. The USA RVZ and cheat-code HTML are reference material, not interchangeable executable assets. Platform details: `docs/linux-arm64.md`.

The port was brought up on macOS ARM64 with Metal (previous workspace `/Users/willi/Projects/mh3`). Everything under "What works" below was verified on that build. The build system now targets arm64 Linux/Vulkan only. It cross-compiles for aarch64, but it has **not yet been run** on arm64 Linux.

**Current priority:** bring the game up on real arm64 Linux hardware with Vulkan. Translate on Linux, build `build/linux-arm64/MH3`, then pass `--self-test`, `--graphics-check` and `--efb-check` and re-verify village/Moga Woods. After that, validate and benchmark the opt-in shared-render-buffer path (eliminates Aurora's staging-buffer → GPU-buffer copy), then optimize GX decode/copy costs and frame pacing. Keep the guest-RAM → render snapshot CPU copy for now.

Last verified implementation commit on macOS: `eccf5f1` (bounded asynchronous frame encoding). Run `git status --short` before editing; do not discard work in progress.

User requirements:

- Commit changes; keep generated PPC at `-O1` unless measured gains justify otherwise.
- Gate debug/instrumentation checks behind build flags, OFF for release.
- Label inspected guest functions in `symbols.txt`; compare suspicious PPC math with Dolphin.
- Use the same post-Chief save and 4:3 presentation for comparisons.
- Use Dolphin's tools/pipe input; do not request desktop/accessibility access. Stop finished runs.
- Dependency builds are authorized. Keep shared WiiCompiled/Aurora sources unchanged; use mh3-local source overlays.

## What works (verified on the macOS/Metal build)

- Native ARM64 boot, title/save menus, Classic Controller input, saved character, village, NPC dialogue and movement.
- Post-Chief save fixture and automatic village replay; no need to repeat the opening conversation.
- Dock collision regression fixed through the translator's paired-single/scalar inference fix (`770ead4`). Building collision still needs a separate gameplay check.
- Field RSO overlays translated; village → Moga Woods works (`5d5e8ec`). Full-game/quest coverage remains unverified.
- Dark top/left rendering border fixed: GX viewport bias must be **342**, not 340, in both decoder and native encoder (`e47ff7b`). Village and field checked.
- Checked RAM fast paths reduce field CPU cost about 57 → 42 ms and frame interval about 67 → 50 ms (`f44f156`).
- Bounded completed-frame encoding/submission worker (`eccf5f1`); main thread retains SDL presentation (now also on Linux). GX decode and mid-frame EFB work remain on the game thread. Async is functional but did not establish a throughput improvement (~20 FPS either way).
- EFB CPU read/write and texture-copy coherence, runtime/audio/controller self-tests. Audible audio quality still needs verification.

Older prose in README/gameplay debugging describes historical failures; use the later reports and this status when deciding what is still broken.

## Build and run on arm64 Linux

Prerequisites: Clang, CMake, Ninja, Python 3, .NET 8 SDK, the SDL X11/Wayland/audio development packages, a Vulkan 1.1+ driver, and extracted EU assets. The full list is in `docs/linux-arm64.md`. Aurora is included at `aurora-main/`. Its common dependencies, including the prebuilt `linux-aarch64` Vulkan Dawn package, use Aurora's providers and are cached under `build/aurora-arm64/_deps`.

```sh
DOTNET_BIN=dotnet ./scripts/translate.sh     # must run on Linux; see docs/linux-arm64.md
./scripts/build-aurora.sh                    # build/linux-arm64/MH3
ctest --test-dir build/linux-arm64 --output-on-failure
build/linux-arm64/MH3 --graphics-check
build/linux-arm64/MH3 --efb-check
./scripts/run.sh --village
```

On a non-arm64 or non-Linux machine the scripts cross-compile with `cmake/toolchains/linux-arm64.cmake` (set `MH3_SYSROOT` or install arm64 multiarch `-dev` packages). `build-aurora.sh` writes `logs/aurora-configure.log` and `logs/aurora-build.log`. MH3 C++ uses `-O1 -gline-tables-only -ffp-contract=off` even in Release. `graphics_host.cpp` requests `BACKEND_VULKAN` and throws if Aurora falls back to anything else.

A/B switches:

```sh
MH3_ASYNC_RENDER=0 ./scripts/run.sh --village
MH3_SHARED_RENDER_BUFFERS=1 ./scripts/run.sh --village
```

The graphics check runs 30 frames and tests shutdown. For shared buffers, check the log line `MH3 direct shared render buffers:` to see whether the direct path was actually enabled. A pass on the fallback path does not validate the optimization.

For final builds, keep debug options off:

```sh
cmake -S runtime -B build/linux-arm64 \
  -DMH3_ENABLE_GPU_VALIDATION=OFF \
  -DMH3_ENABLE_GUEST_CALL_TRACE=OFF \
  -DMH3_ENABLE_COLLISION_CHECK=OFF \
  -DMH3_ENABLE_GX_DIAGNOSTICS=OFF
cmake --build build/linux-arm64 --parallel
```

CPU-only build: `./scripts/build.sh` (`build/linux-arm64-cpu`), then `ctest --test-dir build/linux-arm64-cpu --output-on-failure`. It is not the graphical gameplay executable. Translator build, RVZ extraction and partition preparation are in README. Do not regenerate everything just to change renderer glue.

Keyboard: Space=A, Left Shift=B, WASD=left stick, arrows=D-pad, Enter=Plus. Full mapping: `docs/controller-bringup.md`.

## Repeatable gameplay and Dolphin reference

```sh
python3 scripts/replay_village.py --name next-field --moga-woods --profile --capture-interval 30
# Defaults to build/linux-arm64/MH3; --binary selects another executable
```

Use a unique name. This copies NAND into `logs/replays/NAME/user/nand`, installs `fixtures/post-chief-data00.bin`, and boots through normal loading. It leaves the game running and prints PID, log, capture and controller-file paths. `--dock-edge` reproduces the dock route; `--fresh` replays Chief dialogue. Keep the input file until the process exits. Write states atomically via `controller()` in `scripts/replay_village.py`: hexadecimal buttons, then four decimal stick axes. A=`0x10`; right stick movement on the left stick uses x=511. Clear input after a pulse. Captures are 640×528 BMPs; comparison presentation is 4:3. Stop the specific native PID when finished.

```sh
./scripts/run-dolphin-reference.sh --name ref-next --post-chief --pipe-controller --dump-frames -b
python3 scripts/dolphin-input.py --run ref-next tap A
python3 scripts/dolphin-input.py --run ref-next stick 0 -1 --seconds 1
./scripts/stop-dolphin-reference.sh ref-next
```

On Linux the launcher uses `dolphin-emu` from `PATH` (or `DOLPHIN_BIN`). Dolphin source clone: `build/dolphin-src`. Classic extension + `BackgroundInput=True` are essential. Built-in pipe input works in the installed release; no movie or desktop input automation needed. Reference launcher checks fixture bytes. Optional `--gdb-port 23457`, then `python3 scripts/dolphin-gdb.py 23457`; launcher enables DebugModeEnabled, necessary for JIT guest breakpoints. Starts paused. Commands and savestate/screenshot tooling are described in `docs/gameplay-debugging.md` and script help. Never leave old Dolphin runs consuming resources during benchmarks.

## Shared render buffers (copy removal, unvalidated)

Files: `scripts/aurora-efb.cmake`, `scripts/patch_aurora_shared_buffers.py`. The implementation remains unvalidated. Unrelated `.vscode/` and the cheat HTML should not be swept into commits.

The overlay reuses Aurora's mapped staging ring directly as shader/index/uniform buffers, removing four destination allocations/copies when supported. Ring layout: vertex 3 MiB at 0, uniforms 24 MiB at 3 MiB, indices 2 MiB at 27 MiB, storage 8 MiB at 29 MiB (37 MiB total). Bind groups and tex-copy uniform/index offsets are adjusted. Existing unmap, slot rotation and MapAsync completion protect GPU/CPU ownership; recheck this carefully with mid-frame flushes and async encoding.

It requires Dawn's native `BufferMapExtendedUsages`. The overlay enables adapter toggle `allow_unsafe_apis` on Vulkan and requests the feature whenever it is advertised. Dawn's Vulkan backend advertises it only for a device-local + host-visible memory type (typical of ARM SoC UMA GPUs). The direct path is **opt-in**: `MH3_SHARED_RENDER_BUFFERS=1`. The copy path is the default, and the log states support/request/selection. On macOS/Metal it compiled but was never tested with direct buffers actually enabled; earlier EFB/graphics/CTest passes were fallback-only. Do not report them as zero-copy validation.

`MH3_ENABLE_GPU_VALIDATION` preserves Dawn validation otherwise skipped in Release. Validate normal draws, EFB conversion, ring rollover, mid-frame capacity flush, async on/off and shutdown, then benchmark matched scenes. Document the actual Vulkan memory types/properties, alignment/binding limits and synchronization on the target device; do not infer them from Metal's UMA behaviour.

## arm64 Linux/Vulkan: status and next steps

Done (see `docs/linux-arm64.md`):

- `graphics_host.cpp` requests Vulkan only and throws on any Aurora fallback. Log/error strings name Vulkan.
- `runtime/CMakeLists.txt` accepts only arm64 Linux (`MH3_ALLOW_UNSUPPORTED_HOST=ON` for development) and requires a Dawn with Vulkan. The executable exports symbols for glibc backtraces.
- `cmake/toolchains/linux-arm64.cmake` cross-compiles with Clang. `scripts/target.sh` selects native or cross builds. Build dirs are `build/linux-arm64` and `build/linux-arm64-cpu`.
- The async overlay applies MH3's main-thread presentation model on Linux (upstream Aurora would prepare frames on the worker and present from a presenter thread). The shared-buffer overlay is adapted for Vulkan and is opt-in.
- Cross-compile check (x86-64 host, aarch64 multiarch): Aurora, SDL3 (X11/Wayland/KMSDRM/Vulkan), the MH3 overlays, all runtime sources except `main.cpp`/`abi_bridge.cpp` (which need translator output) and `MH3ControllerHostTest` build for aarch64. The pinned `dawn-linux-aarch64` package contains the Vulkan backend.

Next:

1. On arm64 Linux hardware, run `vulkaninfo --summary` and record the device, driver and memory types.
2. Translate on Linux (generated blob assembly is host-format with absolute `.incbin` paths), build, and run the self-test, graphics, EFB, audio and controller checks.
3. Boot to the village with the post-Chief fixture and compare against Dolphin (same save, 4:3). Then re-check Moga Woods, collisions and audio.
4. Validate and benchmark `MH3_SHARED_RENDER_BUFFERS=1` against the copy path, with validation on for correctness and off for performance.
5. Re-profile on the target (`perf` instead of macOS `sample`). `scripts/summarize_cpu_sample.py` parses macOS `sample` output only.
6. Open question: Dawn is required because Aurora is a WebGPU renderer. Replacing it (wgpu-native, or a native Vulkan backend for Aurora's GX renderer) is not planned.

## Profiling and remaining optimization

Measurements in this section come from the macOS/Metal build and must be repeated on arm64 Linux/Vulkan.

`docs/gameplay-hotspots.md` and `scripts/summarize_cpu_sample.py` hold the latest breakdown. Main-thread sampling in stationary/moving Moga Woods was nearly identical. Moving inclusive samples: GuestGxStream::Write 72.24%, Aurora FIFO processing 40.95%, handle_draw_unmerged 27.58%, gfx::push 20.65%; memcpy/memmove 19.61% self. EFB copy-to-RAM 10.45%, pipeline resolution 7.93%, BindDrawArrays 5.28%. Hashing 6.28% self; all generated guest function self time only 7.28%. Inclusive percentages overlap and must not be added. Completion wait is not a measurement of total GPU execution time.

This is primarily CPU graphics preparation overhead, not proof of a slow collision routine or an ineffective PPC optimizer. Roughly 13k draws/45k array bindings per frame. Investigate redundant upload/binding ranges and hashes after the easy GPU copy removal. Add byte counters before claiming how many bytes copies save. CPU guest-RAM snapshots remain necessary with current ownership; mmap/host-pointer aliasing is separate future research.

Guest graphics wrappers 0x80076A58/0x80087B34/0x8009A0DC have large inclusive but negligible self time. Audio function 0x80475880 is a later readable-rewrite candidate (~2.17% self). Preserve reference-checked numerical behavior for any rewrite.

Other outstanding work:

- Profile slow village → field transition: disc reads, decompression, RSO loading/relocations/checksums, asset setup, texture upload and pipeline compilation. Compare cold/warm against Dolphin.
- Move GX decode/array preparation off-thread only with explicit owned snapshots or guest-write synchronization. Current worker handles completed frame encoding only.
- Verify building collision, further field sections, quests and full module coverage. `docs/module-inventory.md` lists discoverable modules; inventory does not prove all runtime activation paths work.
- Reusable field checkpoints/native savestates, better frame pacing, audible audio validation.

## Source and evidence map

- `runtime/src/graphics_host.cpp`: Vulkan-only backend request, GX handoff, display sealing/completion.
- `runtime/src/gx_stream.cpp`: guest FIFO processing and array bindings.
- `runtime/src/memory.cpp`: guest RAM backing and fast paths.
- `scripts/aurora-efb.cmake`: build-local renderer overlays (do not edit shared Aurora).
- `scripts/patch_aurora_async.py`: worker/main ownership changes (all platforms), fail-closed anchors.
- `scripts/target.sh`, `cmake/toolchains/linux-arm64.cmake`: native vs cross arm64 builds.
- Local Aurora `lib/gfx/common.cpp`: ring, push, bind groups, end-batch copies, frame encode.
- Local Aurora `lib/webgpu/gpu.cpp`: adapter/device features and toggles.
- Local Aurora `lib/gx/pipeline.cpp`, `lib/gfx/tex_copy_conv.cpp`: buffer bindings.
- `symbols.txt`, `docs/function-discovery.md`: guest labels/discovery.
- `docs/async-rendering.md`, `docs/rendering-performance.md`, `TODO.md`: verified work and follow-ups.
- macOS-era evidence: `logs/hotspots-field-{idle,moving}.txt`, `logs/async-field-sample.txt`, `logs/replays/async-render`, `logs/replays/async-off`: local evidence, ignored by Git.

macOS `sample` sometimes freezes a newly launched binary for ~60 seconds during its first warmup and produces only two samples. Do not use that as performance evidence; a subsequent same-process sample worked. Avoid lldb/admin-dependent flows while the user is away. Latest shared-validation replay was stopped; verify process ownership before killing anything else.

## Standalone checkout update

Aurora is now tracked at `aurora-main/`, matching WiiCompiled’s layout. The build defaults to this local copy; `AURORA_ROOT` remains an optional override. The DSP coefficients are tracked at `runtime/assets/dsp/dsp_coef.bin`; the preparation script no longer defaults to the sibling checkout. Common libraries remain normal CMake-managed dependencies, not vendored copies. The sibling WiiCompiled checkout was not modified. On arm64 Linux, Dawn comes from the pinned `linux-aarch64` package with Vulkan. See `docs/aurora-dependency.md`.
