# Runtime flags

Every environment variable the runtime reads. On the Vita, development builds
launched over host0 read `MH3_*`, `AURORA_*` and `SDL_*` lines (`NAME=value`)
from `host0:/game/runtime.env`. Development builds report a variable listed
under [Renamed and removed](#renamed-and-removed) that is still set.

## Settings

| Variable | Effect | Builds |
| --- | --- | --- |
| `MH3_DISC_DIR` | game data directory (default `disc/` next to the executable, written by `--rom`; Vita `ux0:/data/mh3-wii`, whose `disc/` the launcher prepares and then uses) | all |
| `MH3_USER_DIR` | save/NAND root (default `saves` in the working directory; Vita `ux0:/data/mh3-wii/saves`) | all |
| `MH3_NATIVE_ASSET_DIR` | a whole-disc converted texture/model set (`native-textures.bin`, `native-texture-map.bin`, `native-models/`) to use instead of the load-time cache | all |
| `MH3_WIDESCREEN` | `0`: the console's 4:3 setting; default 16:9 ([widescreen](vita-widescreen.md)) | all |
| `MH3_STATS` | `0` turns off the once-per-second statistics lines (default on; [profiling](profiling.md#statistics-lines)); set to anything, it also prints the material-GPU counters | development |
| `MH3_WINDOW_SCALE` | `n`: an n times larger window | development |
| `MH3_RENDER_SCALE` | `n`: render the EFB at n times the native resolution | development |
| `MH3_AUDIO_NULL` | `1`: no playback device; the mix is consumed at 32 kHz | development |
| `MH3_LAUNCHER_INPUT` | launcher input script, on the Vita and with `MH3 --launcher` on a host ([launcher](vita-launcher.md)) | development |
| `MH3_VARIABLE_STEP` | `0`: the game's fixed 1/30 s steps instead of the variable step | development |
| `MH3_RENDER_WIDEN` | `f`: draw every perspective projection `f` times wider than the camera the game culls with (culling edge checks) | development |
| `MH3_DRAW_TABLE_CACHE` | `0`: neither read nor write the draw-table file (`draw-tables.bin`) | all |
| `MH3_GXM_SHADER_DUMP` | a directory: mirror runtime-compiled GXPs (development bank misses, preflights) there; set by the selector preflight | Vita |

## Development tools

Development builds (`MH3_DEVELOPMENT=ON`) have one variable per tool. Its
value is a comma-separated list of items: a flag, `name=value`, or (where the
tool takes one) a list entry such as a path or a port name. An unknown item is
reported at startup (`MH3: MH3_X: unknown option`).

### `MH3_CAPTURE`: frame, audio and RAM dumps

`MH3_CAPTURE=PATH[,interval=N][,start=N][,frame=N][,audio=FILE][,ram=N[,ram-frames=N]]`

| Item | Effect |
| --- | --- |
| `PATH` | BMP frame dumps |
| `interval=N` | every Nth frame, as `PATH`'s name plus `-<frame>.bmp` |
| `start=N` | no interval dumps before frame N |
| `frame=N` | without `interval`: the one frame N (default 30) |
| `audio=FILE` | the audio output as raw s16le stereo (32 kHz from the native mixer) |
| `ram=N` | MEM1 then MEM2 to `mh3-ram-<frame>.bin` in the working directory after paced frame N |
| `ram-frames=N` | with `ram`: after each of N frames from it (default 1) |

Frames count frame packets. `scripts/dev/run-heartbeat.sh` sets
`MH3_CAPTURE=logs/runs/frames/NAME.bmp,interval=150` (`CAPTURE_INTERVAL`
changes the interval).

### `MH3_PORT_CHECK`: native ports against their originals

`MH3_PORT_CHECK=NAME[,NAME...][,every][,dump=DIR][,originals][,KIND:0xADDR...][,fuzz-after=N][,fuzz-samples=N]`
([port checks](../HANDOVER.md#native-ports-and-port-checks), `runtime/include/port_check.h`)

| Item | Effect |
| --- | --- |
| `NAME`, `all` | the ports to check, by their registered name |
| `every` | check every call (default: one check per 500 ms, per port one per 2 s) |
| `dump=DIR` | the first differing GX streams, as `NAME-N-{original,port}.gx` |
| `originals` | sound engine entry points not named run their translated originals ([native audio](native-audio.md#checking-the-engine-port)) |
| `P:0xADDR`, `E:0xADDR`, `C:0xADDR` | fuzz: player and enemy functions called from made-up states (name their ports too); `C:` starts something once |
| `fuzz-after=N` | the fuzz hooks fire on their Nth call (default 900) |
| `fuzz-samples=N` | player fuzz samples per function (default 64) |
| `EffectBatch` | not a port: every material geometry draw's replayed state (its snapshots, colours applied) against the Game state at its submission ([effect batching](native-effects-port.md#batched-particle-draws-and-the-ported-particle-path-2026-10-04)) |
| `CurvesAt` | not a port: the particle curve blend of the owned words against the whole-block blend, at the variable step |

### `MH3_RAZOR`: Vita GPU capture and live metrics

`MH3_RAZOR=live=PATH[,hide-hud][,group=N][,start=N][,frames=N][,scenes][,capture=PATH][,capture-frame=N][,salvage=PATH]`.
The items and their defaults are in [Razor](vita-razor.md);
`scripts/vita-hardware-test.py --wait-razor` reads `live=` from `runtime.env`.

### `MH3_STEP_PROBE`: state that counts steps instead of time

`MH3_STEP_PROBE=T[,T...][,step=N][,settle][,all]`
([variable step](../HANDOVER.md#variable-step)). Each probe writes
`mh3-step-probe-N.txt` to the working directory (never commit these);
`scripts/dev/probefilter.py` filters them.

| Item | Effect |
| --- | --- |
| `T` | probe the first logic step at or after T ticks (1/30 s) of game time |
| `step=N` | probe logic step N |
| `settle` | both runs take one more 1/30 s step, so copies of the previous step's values agree again |
| `all` | probe the whole frame (scene update, tasks, drawing), not just the task pass |

### `MH3_PROFILE`: frame profile, split timers, serial frames

`MH3_PROFILE=[frames=N][,split][,render][,sync][,skip=KIND[+KIND...]][,fill][,unpaced][,disc][,serial-world][,prepare[,prepare-frames=FIRST-LAST]]`

| Item | Effect |
| --- | --- |
| `frames=N` | the legacy frame profile (`MH3 profile:` line) every N frames; only the GLES self-check reaches it now, so game runs print nothing |
| `split` | per-draw split timers in the `MH3 model split` line (two clock reads per draw), the scene processes of model nodes by task in the `MH3 g3d split` line (world matrices, materials, vertices, view matrices, the frame update, the rest), each call of the field's frame update (0x8028CCE0: player control, enemies, camera and the rest) in the `MH3 field split` line, and each 2D layer entry function (the HUD's parts) in the `MH3 layer split` line; game-thread time only |
| `render` | the `MH3 render split` line: the render thread's time per packet in each part of the replay (tasks, frame begin, scene feedback, state restores, model preparation, model draws, material geometry, other geometry, frame end and presentation, waiting). The render thread stores its current part; a sampler thread (core 0 on the Vita) counts it every 250 us, so the render thread reads no clock |
| `sync` | the game thread waits for each frame packet to render (serial frames) |
| `skip=KIND[+KIND...]` | the render thread drops these draw records: `model`, `material` (material geometry: effects, particles), `geometry` (other pre-transformed geometry); for attributing GPU time with Razor |
| `fill` | every 300 frames, the `MH3 material fill` lines: screen area (in screens) that material geometry covers per fragment program and blend, clipped to the viewport |
| `unpaced` | the game thread records its next frame packet even when the render thread has not taken the last one, which the new packet then replaces (the behaviour before pacing; for A/B runs) |
| `disc` | at startup, the `MH3 disc benchmark` lines: the disc image's read speed by read size (32 KiB to 2 MiB) into ordinary and, on the Vita, physically contiguous memory, and the decoder's for 512 KiB reads of the largest model archive with the share spent reading the image (discs read in place through `rom.txt` only) |
| `serial-world` | the scene's world matrix pass runs on the game thread alone instead of split with the guest worker ([guest worker](guest-worker.md); for A/B runs) |
| `prepare` | the model preparation counters and the first 64 source-epoch writers each `MH3 stats` report ([preparation diagnostics](native-model-preparation-diagnostics.md)); `prepare-frames=FIRST-LAST` samples the writers in that range of the game's frame counter |

### `MH3_VERIFY`: native paths against what they replace

`MH3_VERIFY=[pose][,material][,cull][,prep][,draw-table]`

| Item | Effect |
| --- | --- |
| `pose` | every model draw that reuses a pose version compares it with the live view arrays; the render thread checks that each item's version was not reused before it rendered. The `MH3 pose versions` stats line counts the checks and the mismatches ([game-thread copies](game-thread-copies.md)) |
| `material` | every material-GPU draw compares the coefficients its program derives from the Material and Animated blocks with `mh3_shade_fill`, and the SRT rows with the post matrices |
| `cull` | the game-side scene cull draws every model and checks each it would have rejected (`MH3 scene cull` line) |
| `prep` | models are prepared on the game thread (the original path) and the render thread's preparation of the captured inputs is checked against it for every dispatch |
| `draw-table` | indexed draw items resolve by address too, compared with their draw table (`MH3 draw tables` line) |

### `MH3_INPUT`: scripted controller input

`MH3_INPUT=[file=PATH][,record=PATH][,playback=PATH]` ([WPAD bridge](wpad-bridge.md#scripted-input-development-builds))

| Item | Effect |
| --- | --- |
| `file=PATH` | a one-sample control file (hex buttons and four axes), read every frame, replaces the host controller |
| `record=PATH` | logs the host input, one line per change: `frame buttons lx ly rx ry` in 30 Hz frames of game time |
| `playback=PATH` | replays such a log instead of the host input |

### `MH3_TRACE`: game state over game time

`MH3_TRACE=input`

| Item | Effect |
| --- | --- |
| `input` | once per tick of game time: the sticks and game keys as the game read them, the field camera's yaw and pitch, player 0's position and facing, steps and key repeats since the last line ([variable step](variable-step.md#7-input-from-the-host-to-the-game)) |

## Build-option tools

These need a CMake option, which rebuilds the generated code.

### `MH3_WRITE_WATCH`: the guest code storing to memory (`-DMH3_WRITE_WATCH=ON`)

`MH3_WRITE_WATCH=START,LENGTH[,ms=N]` or `MH3_WRITE_WATCH=list=FILE[,ms=N]`

| Item | Effect |
| --- | --- |
| `START,LENGTH` | the range to watch (hex; LENGTH defaults to 4) |
| `list=FILE` | watch the words listed instead (first field of each line, hex: a step probe's output works as is) |
| `ms=N` | report the writers every N ms (default 5000) |

### `MH3_CALL_TRACE`: guest call edges (`-DMH3_ENABLE_GUEST_CALL_TRACE=ON`)

`MH3_CALL_TRACE=PATH[,gate=MARKER]`: the instrumented calls' edges as CSV in
PATH; with `gate`, only once the file MARKER exists. Instrument with
`scripts/trace_guest_calls.py enable`, summarize with `... summarize PATH`
([gameplay debugging](gameplay-debugging.md)); `scripts/replay_village.py
--dock-edge` creates the marker.

`MH3_CALL_TRACE=PATH,movie` in a `-DMH3_ENABLE_FUNCTION_TRACE=ON` build (every
source compiled with `-finstrument-functions-after-inlining`, which slows the
game): every host function call from a movie's first decoded frame until 5 s
after its last. PATH gets each second's calls per callee and call site with the
calling threads; `PATH.seq.N` the game thread's calls and returns in order
around movie N's last frame (its skip and cleanup). `scripts/function_trace.py
PATH` symbolizes both into `PATH.report/`.

## Asset overrides

| Variable | Effect | Builds |
| --- | --- | --- |
| `MH3_NATIVE_MODEL_INDEX` | a converted model index to use instead of `native-models/models.index.bin` | all |
| `MH3_NATIVE_MODEL_PACK` | one converted model pack instead of the index (not with `MH3_NATIVE_MODEL_INDEX`) | all |

The model tests and the asset scripts (`vita-active-play.py`,
`profile_native_host.py`, `capture_native_material_keys.py`) use them.

## Aurora

| Variable | Effect | Builds |
| --- | --- | --- |
| `AURORA_PORT_CAPTURE` | `DIR`: a per-draw state log (`DIR/events.tsv`) and the shader sources (`DIR/shaders/`) | all |
| `AURORA_PORT_CAPTURE_DRAW_INTERVAL` | `N`: log every Nth draw (default 30) | all |
| `AURORA_GROUP_CUTOUTS` | `1`: within each run of depth-tested, depth-writing, unblended draws, draw the opaque ones before the discard (cutout) ones, to cut discard-to-opaque transitions (experimental; coplanar order not validated) | all |

## Read by tools, not the runtime

- `MH3_NV_DEBUG=FUNCTION`: the translator prints which instruction rejects
  each register in its nonvolatile-GPR analysis of that function.
- `MH3_CATALOG_BENCH`: `tests/native/test_mh3_model_catalog.c` also times
  mesh preparation.
- `MH3_FSQRT_TRACE`, `MH3_FSQRT_WARNINGS`, `MH3_FSQRT_END_FRAME`,
  `MH3_LC_CHECK_*`, `MH3_GPU_VERTEX_CAPTURE`, `MH3_GPU_MODELS`,
  `MH3_GPU_CAPTURE_*`: the gdb scripts in `tools/gdb/` (see their headers).
- The scripts' own variables (`MH3_BIN`, `MH3_ARGS`, `MH3_RUN_DIR`,
  `CAPTURE_INTERVAL`, `WATCH_*`, `SETTLE`, `MH3_ARCH`, `MH3_EXTRA_CMAKE_ARGS`,
  `BUILD_JOBS`, `OVERLAY_JOBS`, `MH3_VITA_BUILD_DIR`, `MH3_VITA_ELF_CREATE`)
  are described at the top of each script.

## Renamed and removed

| Was | Now |
| --- | --- |
| `MH3_CAPTURE_PATH`, `_INTERVAL`, `_START`, `_FRAME` | `MH3_CAPTURE=PATH,interval=,start=,frame=` |
| `MH3_AUDIO_DUMP=FILE` | `MH3_CAPTURE=audio=FILE` |
| `MH3_PORT_CHECK_EVERY=1`, `MH3_PORT_CHECK_DUMP=DIR` | `MH3_PORT_CHECK=...,every,dump=DIR` |
| `MH3_PORT_FUZZ=P:0xADDR,...`, `MH3_PORT_FUZZ_AFTER`, `_SAMPLES` | `MH3_PORT_CHECK=...,P:0xADDR,fuzz-after=N,fuzz-samples=N` |
| `MH3_SOUND_ORIGINALS=1` | `MH3_PORT_CHECK=...,originals` |
| `MH3_RAZOR_LIVE`, `_CAPTURE`, `_SALVAGE` | `MH3_RAZOR=live=,capture=,salvage=` |
| `MH3_RAZOR_LIVE_START_FRAME`, `_LIVE_FRAMES`, `_CAPTURE_FRAME`, `_GROUP` | `MH3_RAZOR=start=,frames=,capture-frame=,group=` |
| `MH3_RAZOR_HIDE_HUD=1`, `_SCENE_TRACE=1`, `_CPU_TRACE=1` | `MH3_RAZOR=hide-hud,scenes,cpu` |
| `MH3_STEP_PROBE=N`, `MH3_STEP_PROBE_TICK=T,...` | `MH3_STEP_PROBE=step=N`, `MH3_STEP_PROBE=T,...` |
| `MH3_STEP_PROBE_SETTLE=1`, `MH3_STEP_PROBE_ALL` | `MH3_STEP_PROBE=...,settle,all` |
| `MH3_WRITE_WATCH_LIST=FILE`, `MH3_WRITE_WATCH_MS=N` | `MH3_WRITE_WATCH=list=FILE,ms=N` |
| `MH3_CALL_TRACE_GATE=MARKER` | `MH3_CALL_TRACE=PATH,gate=MARKER` |
| `MH3_PROFILE_FRAMES=N`, `MH3_MODEL_SPLIT=1`, `MH3_RENDER_SYNC=1` | `MH3_PROFILE=frames=N,split,sync` |
| `MH3_MATERIAL_GPU_VERIFY=1`, `MH3_MODEL_CULL_GAME=2`, `MH3_RENDER_PREP=2`, `MH3_DRAW_TABLE=2` | `MH3_VERIFY=material,cull,prep,draw-table` |
| `MH3_RAM_DUMP=FIRST,COUNT` | `MH3_CAPTURE=ram=FIRST,ram-frames=COUNT` |
| `MH3_TRACE_INPUT=1` | `MH3_TRACE=input` |
| `MH3_PREPARE_DIAGNOSTICS=1`, `=FIRST,LAST` | `MH3_PROFILE=prepare`, `MH3_PROFILE=prepare,prepare-frames=FIRST-LAST` |
| `MH3_INPUT_FILE=PATH`, `MH3_INPUT_RECORD=PATH`, `MH3_INPUT_PLAYBACK=PATH` | `MH3_INPUT=file=PATH,record=PATH,playback=PATH` |

Removed with the code behind them (git history keeps them):

- A/B switches of finished work: `MH3_ANIM_WHOLE_FRAMES` (animation at whole
  frames), `MH3_EFFECT_TICK_CURVES` (effect curves at whole ticks),
  `MH3_STEP_COUNTER_RAW` (the step counter counting steps), `MH3_FIELD_WAIT`
  (the field jump's 30-step wait), `MH3_NATIVE_MODEL_DRAW=0` (the translated
  model draw walk; `MH3_PORT_CHECK=G3dDrawOpaXlu` still compares it).
- Rendering and model traces: `MH3_MODEL_TRACE` (model submissions, with
  `scripts/vita-model-submissions.py`), `MH3_MODEL_POSE_TRACE`,
  `MH3_GX_DRAW_LOG`, `MH3_GX_SOURCE_CAPTURE`, `MH3_GX_DEBUG_CONTROL` (with the
  `MH3_ENABLE_GX_DIAGNOSTICS` build option), `MH3_TRACE_EFB`,
  `MH3_MISSING_TARGET_MEM1_DUMP`.
- Simulation traces and checks: `MH3_MOTION_LOG`, `MH3_ROOT_CACHE_CHECK`,
  `MH3_ANM_KEYS_CHECK`, `MH3_ANM_BAKED_CHECK`, `MH3_EFFECT_CURVE_CHECK`,
  `MH3_TICK_SURVEY` (with the `Survey` tick-site kind), `MH3_STEP_LOG`,
  `MH3_STEP_LOG_WORDS`.
- System traces: `MH3_SLOW_PATH_TRACE`, `MH3_AUDIO_TRACE`, and
  `MH3_TRACE_ERRORS` in the IOS, memory and locked-cache code.
- Settings that are now always on, with their alternative paths:
  `MH3_FRAMESKIP=0`, `MH3_MATERIAL_GPU=0`/`models` (the per-draw Shade
  uniforms remain for materials without a GPU block), `MH3_MATERIAL_GPU_RING`,
  `MH3_MODEL_CULL=0`, `MH3_MODEL_CULL_GAME=0`, `MH3_RENDER_PREP=0`,
  `MH3_DRAW_TABLE=0`, `MH3_DRAW_TABLE_DISCOVER=0`, `MH3_DRAW_TABLE_TRACE`,
  `MH3_GXM_ALPHA_ONE=0`, `MH3_GXM_SERIAL_SCENES=1`.
- Diagnostics: `MH3_VCNT` (the fixed step's retrace budget), `MH3_STEP_SCALE`
  (a forced step length), `MH3_TRACE_PLAYER` (`MH3_TRACE=input` prints the
  player's position), `MH3_DRAW_CENSUS`, `MH3_NATIVE_ASSET_TRACE`,
  `MH3_TRACE_ERRORS` (WPAD data formats), `MH3_GXM_SHADE_PROBE`,
  `MH3_GXM_SHADE_DATA_DUMP`, `MH3_GXM_SHADER_ABI_TRACE`, `MH3_GXM_WAIT_TRACE`.
- Earlier: `MH3_MODEL_INSTANCES` (the model-instance path), `MH3_AX_MIX_WORKER`
  and the other AX switches (AX is gone), `MH3_GX_WORKER`, `AURORA_GL_VSYNC`.
