# MH3 handover — 2026-09-27

## Start here

Workspace: `/Users/willi/Projects/mh3` (macOS ARM64). This is a static PowerPC → C++ recompilation of Monster Hunter Tri, using Aurora/Dawn for graphics, not a Dolphin frontend. There is no interpreter/JIT fallback. The active translated game is **European RMHP08**; the USA RVZ and cheat-code HTML are reference material, not interchangeable executable assets.

**Current priority:** use **Vulkan through MoltenVK**, explicitly document required GPU features, and eliminate Aurora's staging-buffer → GPU-buffer copy. Preserve the necessary guest-RAM → render snapshot CPU copy for now. After that, optimize dominant GX decoding/copying costs and frame pacing. The Vulkan switch is requested work, **not implemented yet**.

Last verified implementation commit: `eccf5f1` (bounded asynchronous Metal frame encoding). This handover describes both that baseline and the subsequent unfinished implementation checkpoint. Run `git status --short` before editing; do not discard the work in progress.

User requirements:

- Commit changes; keep generated PPC at `-O1` unless measured gains justify otherwise.
- Gate debug/instrumentation checks behind build flags, OFF for release.
- Label inspected guest functions in `symbols.txt`; compare suspicious PPC math with Dolphin.
- Use the same post-Chief save and 4:3 presentation for comparisons.
- Use Dolphin's tools/pipe input; do not request desktop/accessibility access. Stop finished runs.
- Brew/dependency builds are authorized. Keep shared WiiCompiled/Aurora sources unchanged; use mh3-local source overlays.

## What works

- Native ARM64 boot, title/save menus, Classic Controller input, saved character, village, NPC dialogue and movement.
- Post-Chief save fixture and automatic village replay; no need to repeat the opening conversation.
- Dock collision regression fixed through the translator's paired-single/scalar inference fix (`770ead4`). Building collision still needs a separate gameplay check.
- Field RSO overlays translated; village → Moga Woods works (`5d5e8ec`). Full-game/quest coverage remains unverified.
- Dark top/left rendering border fixed: GX viewport bias must be **342**, not 340, in both decoder and native encoder (`e47ff7b`). Village and field checked.
- Checked RAM fast paths reduce field CPU cost about 57 → 42 ms and frame interval about 67 → 50 ms (`f44f156`).
- Bounded completed-frame encoding/submission worker (`eccf5f1`); main thread retains SDL/AppKit presentation. GX decode and mid-frame EFB work remain on the game thread. Async is functional but did not establish a throughput improvement (~20 FPS either way).
- EFB CPU read/write and texture-copy coherence, runtime/audio/controller self-tests. Audible audio quality still needs verification.

Older prose in README/gameplay debugging describes historical failures; use the later reports and this status when deciding what is still broken.

## Build and run the verified Metal baseline

Prerequisites already local: Clang (Homebrew LLVM), CMake, Ninja, Python 3, project-local .NET SDK, extracted EU assets. Aurora is `/Users/willi/Projects/Wiicompiled/aurora-main`. Build dependencies/assets are ignored by Git, so a new machine needs extraction and dependency setup from README.

```sh
cd /Users/willi/Projects/mh3
./scripts/build-aurora.sh
ctest --test-dir build/metal --output-on-failure
./scripts/run.sh --village
```

`build-aurora.sh` configures `runtime/`, builds `build/metal/MH3` and controller test, and writes `logs/aurora-configure.log` / `logs/aurora-build.log`. It uses Release, Dawn package, SDL and dependencies cached under `build/aurora/_deps`. Existing CMake cache values persist: **the current local build has GPU validation ON** for copy-removal testing. Despite Release, MH3 C++ explicitly uses `-O1 -gline-tables-only -ffp-contract=off`.

Current working-tree overlays are applied by that build command; it is not automatically a pristine baseline. To compare the unfinished copy path with its fallback:

```sh
MH3_SHARED_RENDER_BUFFERS=0 ./scripts/run.sh --village
MH3_ASYNC_RENDER=0 ./scripts/run.sh --village
build/metal/MH3 --efb-check
build/metal/MH3 --graphics-check
```

The graphics check runs 30 frames and tests shutdown. Inspect the log for whether shared render buffers actually enabled; passing with fallback does not validate the optimization.

For normal final builds, explicitly turn off debug options after validation:

```sh
cmake -S runtime -B build/metal \
  -DMH3_ENABLE_GPU_VALIDATION=OFF \
  -DMH3_ENABLE_GUEST_CALL_TRACE=OFF \
  -DMH3_ENABLE_COLLISION_CHECK=OFF \
  -DMH3_ENABLE_GX_DIAGNOSTICS=OFF
cmake --build build/metal --parallel 6
```

CPU-only build: `./scripts/build.sh`, then `ctest --test-dir build/native --output-on-failure`. It is not the graphical gameplay executable. Translator rebuild, RVZ extraction, partition preparation and regeneration commands are in README. Do not regenerate everything just to change renderer glue.

Keyboard: Space=A, Left Shift=B, WASD=left stick, arrows=D-pad, Enter=Plus. Full mapping: `docs/controller-bringup.md`.

## Repeatable gameplay and Dolphin reference

```sh
python3 scripts/replay_village.py --name next-field --moga-woods --profile --capture-interval 30
# For a future Vulkan binary, append: --binary build/vulkan/MH3
```

Use a unique name. This copies NAND into `logs/replays/NAME/user/nand`, installs `fixtures/post-chief-data00.bin`, and boots through normal loading. It leaves the game running and prints PID, log, capture and controller-file paths. `--dock-edge` reproduces the dock route; `--fresh` replays Chief dialogue. Keep the input file until the process exits. Write states atomically via `controller()` in `scripts/replay_village.py`: hexadecimal buttons, then four decimal stick axes. A=`0x10`; right stick movement on the left stick uses x=511. Clear input after a pulse. Captures are 640×528 BMPs; comparison presentation is 4:3. Stop the specific native PID when finished.

```sh
./scripts/run-dolphin-reference.sh --name ref-next --post-chief --pipe-controller --dump-frames -b
python3 scripts/dolphin-input.py --run ref-next tap A
python3 scripts/dolphin-input.py --run ref-next stick 0 -1 --seconds 1
./scripts/stop-dolphin-reference.sh ref-next
```

Dolphin source clone: `build/dolphin-src`. Classic extension + `BackgroundInput=True` are essential. Built-in pipe input works in the installed release; no movie or desktop input automation needed. Reference launcher checks fixture bytes. Optional `--gdb-port 23457`, then `python3 scripts/dolphin-gdb.py 23457`; launcher enables DebugModeEnabled, necessary for JIT guest breakpoints. Starts paused. Commands and savestate/screenshot tooling are described in `docs/gameplay-debugging.md` and script help. Never leave old Dolphin runs consuming resources during benchmarks.

## Copy-removal work-in-progress checkpoint

Files: `runtime/CMakeLists.txt`, `scripts/aurora-efb.cmake`, new `scripts/patch_aurora_shared_buffers.py`. These are saved in a separate **work-in-progress implementation commit** after the handover documentation commit: implementation remains unvalidated. Unrelated `.vscode/` and the cheat HTML should not be swept into commits.

The overlay reuses Aurora's mapped staging ring directly as shader/index/uniform buffers, removing four destination allocations/copies when supported. Ring layout: vertex 3 MiB at 0, uniforms 24 MiB at 3 MiB, indices 2 MiB at 27 MiB, storage 8 MiB at 29 MiB (37 MiB total). Bind groups and tex-copy uniform/index offsets are adjusted. Existing unmap, slot rotation and MapAsync completion protect GPU/CPU ownership; recheck this carefully with mid-frame flushes and async encoding.

Requires Dawn native `BufferMapExtendedUsages`; the first tests silently selected fallback because the experimental feature was hidden. The latest edit adds adapter toggle `allow_unsafe_apis`, requests the feature and compiles successfully (`logs/shared-build2.log`), but **has not yet been tested with direct buffers actually enabled**. Earlier EFB/graphics/CTest passes were fallback-only (`logs/shared-efb.log`, `logs/shared-graphics.log`). Do not report them as zero-copy validation.

Current feature detection/request is Metal-specific and must be adapted for Vulkan. `MH3_SHARED_RENDER_BUFFERS=0` is the comparison fallback. New `MH3_ENABLE_GPU_VALIDATION` preserves Dawn validation otherwise skipped in Release; currently ON in build/metal. Need validate normal draws, EFB conversion, ring rollover, mid-frame capacity flush, async on/off and shutdown, then benchmark matched scenes.

## Vulkan migration: exact next steps

1. `runtime/src/graphics_host.cpp` currently requests **BACKEND_METAL** and asserts Metal (around lines 99/116). Add explicit backend selection and ensure requested Vulkan does not silently fall back.
2. Bundled Dawn package is macOS Metal/Null only. Aurora's `cmake/AuroraDawnProvider.cmake` also infers Vulkan OFF for Apple packages. Build vendor Dawn with Vulkan enabled in a separate `build/vulkan` tree; preserve Metal as a comparison build.
3. Installed successfully via brew: `vulkan-loader`, `vulkan-headers`, `vulkan-tools`; MoltenVK already installed at `/opt/homebrew/opt/molten-vk` (`/opt/homebrew/lib/libMoltenVK.dylib`). Installation log: `logs/vulkan-packages.log`. Locate the MoltenVK ICD JSON and run `vulkaninfo --summary` against that ICD, not Vulkan tools' mock ICD.
4. Pinned package version is `v20260603.191052`, downloaded from **theofficialgman/dawn-build** releases. That is NOT a google/dawn tag: the attempted Google tag tarball returned 404. `build/dawn-vulkan-src` is currently empty. Resolve the release's actual upstream source commit, pin it and build reproducibly. Aurora vendor mode can use `FETCHCONTENT_SOURCE_DIR_DAWN` to avoid its incorrect tag URL for this package version.
5. Inspect Dawn Vulkan support for `BufferMapExtendedUsages` on this MoltenVK device. Document actual required features, memory types/properties, alignment/binding limits and synchronization. Do not infer Vulkan capability from Metal's UMA support. Make the direct-buffer path capability-gated, retain fallback and clearly log selection.
6. Add a reproducible Vulkan build/run script; check surface creation/presentation, shader translation, async behavior and EFB tests. Vulkan still uses Metal underneath MoltenVK, but exposes the explicit portable API the user wants.
7. Run matched shared-buffer ON/OFF benchmarks with validation on for correctness, off for performance. Commit working implementation plus measured results and feature list.

## Profiling and remaining optimization

`docs/gameplay-hotspots.md` and `scripts/summarize_cpu_sample.py` hold the latest breakdown. Main-thread sampling in stationary/moving Moga Woods was nearly identical. Moving inclusive samples: GuestGxStream::Write 72.24%, Aurora FIFO processing 40.95%, handle_draw_unmerged 27.58%, gfx::push 20.65%; memcpy/memmove 19.61% self. EFB copy-to-RAM 10.45%, pipeline resolution 7.93%, BindDrawArrays 5.28%. Hashing 6.28% self; all generated guest function self time only 7.28%. Inclusive percentages overlap and must not be added. Completion wait is not a measurement of total GPU execution time.

This is primarily CPU graphics preparation overhead, not proof of a slow collision routine or an ineffective PPC optimizer. Roughly 13k draws/45k array bindings per frame. Investigate redundant upload/binding ranges and hashes after the easy GPU copy removal. Add byte counters before claiming how many bytes copies save. CPU guest-RAM snapshots remain necessary with current ownership; mmap/host-pointer aliasing is separate future research.

Guest graphics wrappers 0x80076A58/0x80087B34/0x8009A0DC have large inclusive but negligible self time. Audio function 0x80475880 is a later readable-rewrite candidate (~2.17% self). Preserve reference-checked numerical behavior for any rewrite.

Other outstanding work:

- Profile slow village → field transition: disc reads, decompression, RSO loading/relocations/checksums, asset setup, texture upload and pipeline compilation. Compare cold/warm against Dolphin.
- Move GX decode/array preparation off-thread only with explicit owned snapshots or guest-write synchronization. Current worker handles completed frame encoding only.
- Verify building collision, further field sections, quests and full module coverage. `docs/module-inventory.md` lists discoverable modules; inventory does not prove all runtime activation paths work.
- Reusable field checkpoints/native savestates, better frame pacing, audible audio validation.

## Source and evidence map

- `runtime/src/graphics_host.cpp`: backend selection, GX handoff, display sealing/completion.
- `runtime/src/gx_stream.cpp`: guest FIFO processing and array bindings.
- `runtime/src/memory.cpp`: guest RAM backing and fast paths.
- `scripts/aurora-efb.cmake`: build-local renderer overlays (do not edit shared Aurora).
- `scripts/patch_aurora_async.py`: worker/main ownership changes, fail-closed anchors.
- Shared Aurora `lib/gfx/common.cpp`: ring, push, bind groups, end-batch copies, frame encode.
- Shared Aurora `lib/webgpu/gpu.cpp`: adapter/device features and toggles.
- Shared Aurora `lib/gx/pipeline.cpp`, `lib/gfx/tex_copy_conv.cpp`: buffer bindings.
- `symbols.txt`, `docs/function-discovery.md`: guest labels/discovery.
- `docs/async-rendering.md`, `docs/rendering-performance.md`, `TODO.md`: verified work and follow-ups.
- `logs/hotspots-field-{idle,moving}.txt`, `logs/async-field-sample.txt`, `logs/replays/async-render`, `logs/replays/async-off`: local evidence, ignored by Git.

macOS `sample` sometimes freezes a newly launched binary for ~60 seconds during its first warmup and produces only two samples. Do not use that as performance evidence; a subsequent same-process sample worked. Avoid lldb/admin-dependent flows while the user is away. Latest shared-validation replay was stopped; verify process ownership before killing anything else.
