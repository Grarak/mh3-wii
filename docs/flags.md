# Runtime flags

Every environment variable the runtime reads (2026-10-03). The Vita reads
them from `host0:/game/runtime.env` (one `NAME=value` per line) in
development builds launched over host0.

## Settings

| Variable | Effect | Builds |
| --- | --- | --- |
| `MH3_DISC_DIR` | prepared game data (default `disc/` next to the executable; Vita `ux0:/data/mh3-wii`) | all |
| `MH3_USER_DIR` | save/NAND root (default `saves`; Vita `ux0:/data/mh3-wii/saves`) | all |
| `MH3_NATIVE_ASSET_DIR` | a complete native texture/model set to use instead of the one in `MH3_DISC_DIR` | all |
| `MH3_WIDESCREEN` | `0`: the console's 4:3 setting; default 16:9 ([widescreen](vita-widescreen.md)) | all |
| `MH3_STATS` | `0` turns off the once-per-second `MH3 stats` lines (default on) | development |
| `MH3_FRAMESKIP` | `0` turns off frameskip | development |
| `MH3_WINDOW_SCALE` | `n`: an n times larger window | development |
| `MH3_RENDER_SCALE` | `n`: render the EFB at n times the native resolution | development |
| `MH3_AUDIO_NULL` | `1`: no playback device; the mix is consumed at 32 kHz | development |
| `MH3_LAUNCHER_INPUT` | Vita launcher input script ([launcher](vita-launcher.md)) | development |

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

`scripts/dev/run-heartbeat.sh` sets `MH3_CAPTURE=logs/runs/frames/NAME.bmp,interval=150`
(`CAPTURE_INTERVAL` changes the interval).

### `MH3_PORT_CHECK`: native ports against their originals

`MH3_PORT_CHECK=NAME[,NAME...][,every][,dump=DIR][,originals][,KIND:0xADDR...][,fuzz-after=N][,fuzz-samples=N]`
([port checks](../HANDOVER.md#native-ports-and-port-checks), `runtime/include/port_check.h`)

| Item | Effect |
| --- | --- |
| `NAME`, `all` | the ports to check, by their registered name |
| `every` | check every call (default: one check per 500 ms, per port one per 2 s) |
| `dump=DIR` | the first differing GX streams, as `NAME-N-{original,port}.gx` |
| `originals` | sound engine entry points not named run their translated originals ([native audio](native-audio.md#checking-the-engine-port)) |
| `P:0xADDR`, `E:0xADDR`, `C:0xADDR` | fuzz: player and enemy functions called from made-up states; `C:` starts something once |
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
| `frames=N` | the frame profile (wall/CPU ms per frame, completion, queue) and GX counters every N frames |
| `split` | per-draw split timers in the `MH3 model split` line (two clock reads per draw: ~1.5 ms/frame on the Vita) |
| `sync` | the game thread waits for each frame packet to render (serial frames) |

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

## Renamed and removed

A development build names any of these that is still set at startup.

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

Removed on 2026-10-03 with the code behind them (git history keeps them):

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
