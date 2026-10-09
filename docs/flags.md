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
| `MH3_FRAMESKIP` | `0` turns off frameskip | development |
| `MH3_WINDOW_SCALE` | `n`: an n times larger window | development |
| `MH3_RENDER_SCALE` | `n`: render the EFB at n times the native resolution | development |
| `MH3_AUDIO_NULL` | `1`: no playback device; the mix is consumed at 32 kHz | development |
| `MH3_LAUNCHER_INPUT` | launcher input script, on the Vita and with `MH3 --launcher` on a host ([launcher](vita-launcher.md)) | development |

## Development tools

Development builds (`MH3_DEVELOPMENT=ON`) have one variable per tool. Its
value is a comma-separated list of items: a flag, `name=value`, or (where the
tool takes one) a list entry such as a path or a port name. An unknown item is
reported at startup (`MH3: MH3_X: unknown option`).

### `MH3_CAPTURE`: frame and audio dumps

`MH3_CAPTURE=PATH[,interval=N][,start=N][,frame=N][,audio=FILE]`

| Item | Effect |
| --- | --- |
| `PATH` | BMP frame dumps |
| `interval=N` | every Nth frame, as `PATH`'s name plus `-<frame>.bmp` |
| `start=N` | no interval dumps before frame N |
| `frame=N` | without `interval`: the one frame N (default 30) |
| `audio=FILE` | the audio output as raw s16le stereo (32 kHz from the native mixer) |

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

`MH3_PROFILE=[frames=N][,split][,sync]`

| Item | Effect |
| --- | --- |
| `frames=N` | the legacy frame profile (`MH3 profile:` line) every N frames; only the GLES self-check reaches it now, so game runs print nothing |
| `split` | per-draw split timers in the `MH3 model split` line (two clock reads per draw) |
| `sync` | the game thread waits for each frame packet to render (serial frames) |

### `MH3_VERIFY`: reused state against a fresh copy

`MH3_VERIFY=pose` ([game-thread copies](game-thread-copies.md))

| Item | Effect |
| --- | --- |
| `pose` | every model draw that reuses a pose version compares it with the live view arrays; the render thread checks that each item's version was not reused before it rendered. The `MH3 pose versions` stats line counts the checks and the mismatches |

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

## Not yet merged

A second cleanup pass merges or removes these; the last column says how.

| Variable | Effect | Builds | Plan |
| --- | --- | --- | --- |
| `MH3_INPUT_FILE`, `MH3_INPUT_RECORD`, `MH3_INPUT_PLAYBACK` | scripted controller input; record and play back | development | one input family |
| `MH3_VARIABLE_STEP` | `0`: the game's fixed 1/30 s steps | development | keep |
| `MH3_VCNT` | `n`: the fixed step's retrace budget | development | remove |
| `MH3_STEP_SCALE` | `s`: every variable step is s ticks long | development | remove |
| `MH3_RAM_DUMP` | `FIRST,COUNT`: MEM1+MEM2 to `mh3-ram-<frame>.bin` after COUNT frames from FIRST | development | into `MH3_CAPTURE` |
| `MH3_TRACE_PLAYER` | player 0's position and motion by game time | development | remove |
| `MH3_TRACE_INPUT` | once per tick of game time: the sticks and game keys as the game read them, the field camera's yaw and pitch, player 0's position and facing, steps and key repeats since the last line ([variable step](variable-step.md#7-input-from-the-host-to-the-game)) | development | with `MH3_TRACE_PLAYER` into one trace tool |
| `MH3_RENDER_PREP` | model preparation: `0` game thread, `1` (default) render thread, `2` both, checked | all (`2`: development) | `2` into `MH3_VERIFY` |
| `MH3_DRAW_TABLE` | draw items: `1` (default) through the model's draw table, `0` by address, `2` both, compared | all (`2`: development) | `2` into `MH3_VERIFY` |
| `MH3_DRAW_TABLE_CACHE` | `0`: neither read nor write the draw-table file | all | keep or remove after its A/B |
| `MH3_DRAW_TABLE_DISCOVER` | `0`: draw tables only for models already drawn | all | remove |
| `MH3_DRAW_TABLE_TRACE` | draw-table statistics | development | remove |
| `MH3_MATERIAL_GPU` | `0`: the per-draw Shade uniforms instead of material blocks in GPU memory | all | remove (on in the devkit A/B) |
| `MH3_MATERIAL_GPU_RING` | route the render thread's Animated sections through the producer ring (self-test) | development | remove |
| `MH3_MATERIAL_GPU_VERIFY` | compare the material programs' coefficients with `mh3_shade_fill` | development | into `MH3_VERIFY` |
| `MH3_MODEL_CULL` | `0`: draw models whose mesh box is off screen | all | remove (on in the devkit A/B) |
| `MH3_MODEL_CULL_GAME` | `0`: no game-side culling of off-screen scene models; `2` (development): draw everything and check every rejected model | all | `2` into `MH3_VERIFY`, remove `0` |
| `MH3_RENDER_WIDEN` | `f`: draw every perspective projection `f` times wider than the camera the game culls with (edge check) | development | keep for culling checks |
| `MH3_DRAW_CENSUS` | `N`: the model draw pass's owners every N frames | development | remove |
| `MH3_PREPARE_DIAGNOSTICS` | model preparation diagnostics for a frame range | development | remove |
| `MH3_NATIVE_ASSET_TRACE` | model catalog registration log (`model_catalog.cpp`) | development | remove |
| `MH3_TRACE_ERRORS` | WPAD data-format log (`wpad_services.cpp`) | all | remove |
| `MH3_GXM_ALPHA_ONE` | `0`: no alpha-one shortcut for shaded draws | Vita | remove |
| `MH3_GXM_SERIAL_SCENES` | `1`: serialize GXM scenes | Vita | remove |
| `MH3_GXM_SHADE_PROBE` | replace a shaded program's output stage (`blue`, ...) | Vita | remove |
| `MH3_GXM_SHADE_DATA_DUMP` | dump shaded programs' uniform blocks | Vita | remove |
| `MH3_GXM_SHADER_ABI_TRACE` | log each GXM program's reflection once | Vita | remove |
| `MH3_GXM_WAIT_TRACE` | log the first `sceGxmFinish` waits | Vita | remove |
| `MH3_GXM_SHADER_DUMP` | mirror runtime-compiled GXPs (development bank misses, preflights) to a directory; set by the selector preflight | Vita | keep for the preflight |

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
- Earlier: `MH3_MODEL_INSTANCES` (the model-instance path), `MH3_AX_MIX_WORKER`
  and the other AX switches (AX is gone), `MH3_GX_WORKER`, `AURORA_GL_VSYNC`.
