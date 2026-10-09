# Runtime flags

Every environment variable the runtime reads. Most are diagnostics compiled
only into development builds (`MH3_DEVELOPMENT=ON`; the build scripts make
release builds unless it is set); the Builds
column says which.

## Settings

| Variable | Effect | Builds |
| --- | --- | --- |
| `MH3_DISC_DIR` | game data directory (default `disc/` next to the executable, written by `--rom`; Vita `ux0:/data/mh3-wii`, whose `disc/` the launcher prepares and then uses) | all |
| `MH3_USER_DIR` | save/NAND root (default `saves` in the working directory; Vita `ux0:/data/mh3-wii/saves`) | all |
| `MH3_NATIVE_ASSET_DIR` | a whole-disc converted texture/model set (`native-textures.bin`, `native-texture-map.bin`, `native-models/`) to use instead of the load-time cache | all |
| `MH3_WIDESCREEN` | `0`: the console's 4:3 setting; default 16:9 ([widescreen](optimizations/rendering/widescreen.md)) | all |
| `MH3_FRAME_LIMIT` | `off` turns the game thread limiter off (and on the Vita flips without VSync), as the launcher's `Framelimit: OFF` does, for runs started without the launcher: benchmarks show the real speed | all |
| `MH3_MSAA` | `2` or `4` renders the EFB with 2x or 4x multisampling (the Vita's GXM renderer, and the GL one where the driver offers it), `off` without, as the launcher's `MSAA` row does, for runs started without the launcher | development |
| `MH3_STATS` | `0` turns off the once-per-second statistics lines (default on; [profiling](profiling.md#statistics-lines)); set to anything, it also prints the material-GPU counters | development |
| `MH3_WINDOW_SCALE` | `n`: an n times larger window | development |
| `MH3_RENDER_SCALE` | `n`: render the EFB at n times the native resolution (448·n rows); overrides the launcher's `Upscaling` ([launcher](vita-launcher.md)) | development |
| `MH3_THP_JPEG` | `off`: the Vita decodes the movies in software instead of on its hardware JPEG decoder, frames ahead ([movies](optimizations/assets/hardware-movie-decoding.md)) | development |
| `MH3_AUDIO_NULL` | `1`: no playback device; the mix is consumed at 32 kHz | development |
| `MH3_LAUNCHER_INPUT` | launcher input script, on the Vita and with `MH3 --launcher` on a host ([launcher](vita-launcher.md)) | development |
| `MH3_VARIABLE_STEP` | `0`: the game's fixed 1/30 s steps instead of the variable step | development |
| `MH3_GXM_SHADER_DUMP` | a directory: mirror runtime-compiled GXPs (development builds' bank misses) there with their Cg | Vita |
| `MH3_LANGUAGE` | `en`, `fr`, `de`, `it` or `es`: the game's language (the console's `IPL.LNG`), as the launcher's `Language` row does, for runs started without the launcher | all |
| `MH3_ONLINE_SERVER` | `DOMAIN[:PORT]` or `IP[:PORT]`: the online server (default `mh3sp.com`), as the launcher's online server setting does ([online](online.md)) | all |
| `MH3_ONLINE_SKIP_TLS` | `1`: TLS sessions pass their bytes through unencrypted (a local MH3SP with `UseSSL = OFF`; [online](online.md)) | all |
| `MH3_ONLINE_CA` | a DER file: the root CA the game pins for its Capcom server instead of the MH3SP one (a local MH3SP server's `cert/ca.der`; [online](online.md)) | all |
| `MH3_ONLINE_HOSTS` | `name=address[,...]` (`*.domain` for every host in it): answer these lookups without DNS ([online](online.md)) | Linux |
| `MH3_ONLINE_PORTS` | `from=to[,...]`: connect to port `to` instead of `from` (a local account server instead of port 80; [online](online.md)) | Linux |
| `MH3_ONLINE_ADDRESSES` | `from=to[,...]`: reach IPv4 address `from` at `to`, and read packets from `to` as from `from` (a server that names its ZeroTier address, reached from a Vita on the LAN; [online](online.md)) | Linux |
| `MH3_ONLINE_AUTO` | `id=N,server=N,gate=N,city=N,post=QUEST[,members=N][,depart=N]` or `...,join=N`: the online autopilot takes a `--city` launch through the online screens into a quest without input (`city=-1`: it stays in the gate, Loc Lac's streets) (`runtime/src/net/online_auto.cpp`) | development |

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

Frames count frame packets.

### `MH3_PORT_CHECK`: native ports against their originals

`MH3_PORT_CHECK=NAME[,NAME...][,every][,dump=DIR][,originals][,KIND:0xADDR...][,fuzz-after=N][,fuzz-samples=N][,fuzz-demo=V/V...][,fuzz-frames=V/V...]`
([native function ports](optimizations/cpu/native-function-ports.md), `runtime/include/port_check.h`)

| Item | Effect |
| --- | --- |
| `NAME`, `all` | the ports to check, by their registered name |
| `every` | check every call (default: one check per 500 ms, per port one per 2 s) |
| `dump=DIR` | the first differing GX streams, as `NAME-N-{original,port}.gx` |
| `originals` | sound engine entry points not named run their translated originals ([native audio](optimizations/system/native-audio.md)) |
| `tolerance=R` | approximated ports: two differing words that read as finite floats within R of each other (relative to the larger, absolute below 1) count as close, not different; a `words within the tolerance` line counts them |
| `P:0xADDR`, `E:0xADDR`, `C:0xADDR` | fuzz: player and enemy functions called from made-up states (name their ports too); `C:` starts something once |
| `fuzz-after=N` | the fuzz hooks fire on their Nth call (default 900) |
| `A:0xADDR[:KIND]` | fuzz an enemy action: run once (from the player's hook, from `fuzz-after` on) on an active enemy of that kind (+3), one in the current area if any, over 4 × `fuzz-samples` states (+5 each of 0-15, substate, count +0x20, motion frame, the event demo clock, the step counter); for actions only an event demo reaches (`monster_intros.cpp`) or that no organic run reaches soon (name their ports too) |
| `fuzz-samples=N` | player fuzz samples per function (default 64; the A: fuzz runs four times as many) |
| `fuzz-demo=V/V/...` | A: fuzz: every fourth run of 16 samples sets the event demo clock to one of these steps (for `steps == K` tests) |
| `fuzz-frames=V/V/...` | A: fuzz: every other run of 16 samples puts the motion on (or half a frame or a frame before) one of these frames |

While `MH3_PORT_CHECK` is set nothing runs beside the game thread: the effect
calculation stays on it, the world matrix pass is not split (but for
`ScnGroupCalcWorld`), the scene graph walk stays on it and the next logic step
waits for each frame's draw ([pipelined draw](optimizations/threading/pipelined-draw.md)).

### `MH3_STEP_PROBE`: state that counts steps instead of time

`MH3_STEP_PROBE=T[,T...][,step=N][,settle][,all]`
([variable step](optimizations/simulation/variable-step.md)). Each probe writes
`mh3-step-probe-N.txt` to the working directory.

| Item | Effect |
| --- | --- |
| `T` | probe the first logic step at or after T ticks (1/30 s) of game time |
| `step=N` | probe logic step N |
| `settle` | both runs take one more 1/30 s step, so copies of the previous step's values agree again |
| `all` | probe the whole frame (scene update, tasks, drawing), not just the task pass |

### `MH3_PROFILE`: split timers, serial frames

`MH3_PROFILE=[split][,render][,sync][,skip=KIND[+KIND...]][,fill][,disc][,census]`

| Item | Effect |
| --- | --- |
| `split` | per-draw split timers in the `MH3 model split` line (two clock reads per draw), the scene processes of model nodes by task in the `MH3 g3d split` line (world matrices, materials, vertices, view matrices, the frame update, the rest), each call of the field's frame update (0x8028CCE0: player control, enemies, camera and the rest) in the `MH3 field split` line, and each 2D layer entry function (the HUD's parts) in the `MH3 layer split` line; game-thread time only |
| `render` | the `MH3 render split` line: the render thread's time per packet in each part of the replay (tasks, frame begin, scene feedback, state restores, model preparation, model draws, material geometry, other geometry, frame end and presentation, waiting). The render thread stores its current part; a sampler thread (core 0 on the Vita) counts it every 250 us, so the render thread reads no clock |
| `sync` | the game thread waits for each frame packet to render (serial frames): each thread's frame time without the other beside it |
| `skip=KIND[+KIND...]` | the render thread drops these draw records: `model`, `material` (material geometry: effects, particles), `geometry` (other pre-transformed geometry), or meshes of model draws: skinned ones (`skinned`), those whose material blends (`blended`) or alpha tests (`discard`) |
| `fill` | every 300 frames, the `MH3 material fill` lines: screen area (in screens) that material geometry covers per fragment program and blend, clipped to the viewport |
| `disc` | at startup, the `MH3 disc benchmark` lines: the disc image's read speed by read size (32 KiB to 2 MiB) into ordinary and, on the Vita, physically contiguous memory, and the decoder's for 512 KiB reads of the largest model archive with the share spent reading the image (discs read in place through `rom.txt` only) |
| `census` | every 240 frames, the `MH3 draw census` lines: per frame, the model draws, the skinned palette-batch draws, the distinct index ranges and meshes (a mesh split into palette batches draws once per batch), runs of draws sharing program, material and asset, the ten assets with the most draws, and the sixteen programs (vertex+fragment stage names) with the most triangles, with their draws |

Every item measures or reports (`sync` and `skip` change the timing or the
render thread's work only to attribute time). The A/B items of paths every build now takes were removed (a
stale one prints `MH3: MH3_PROFILE: '...' is ignored; it was removed`):
`draw-inline` (the whole frame drawn on the game thread), `draw-thread` (the
scene on the draw thread while the game thread waits), `serial-prepare` (the
scene graph walk on the game thread), `serial-world` (the world matrix pass on
the game thread alone; `MH3_PORT_CHECK` other than `ScnGroupCalcWorld` still
keeps it there), `queue=N` (two packets waiting for the render thread) and
`unpaced` (the launcher's `Framelimit: OFF` does the same; `MH3_FRAME_LIMIT=off` for runs started without the launcher).

### `MH3_INPUT`: scripted controller input

`MH3_INPUT=[file=PATH][,record=PATH][,playback=PATH][,camera=N[:D]]` ([WPAD bridge](optimizations/system/controller-input.md))

| Item | Effect |
| --- | --- |
| `file=PATH` | a one-sample control file (hex buttons and four axes), read every frame, replaces the host controller |
| `record=PATH` | logs the host input, one line per change: `frame buttons lx ly rx ry` in 30 Hz frames of game time |
| `playback=PATH` | replays such a log instead of the host input |
| `camera=N[:D]` | (development builds) the field camera looks at monster N (its move work, `MH3_TRACE=monsters`): from the game's eye, or from D units away on the line to it, raised by D/3. The camera work keeps its own eye and target |

### `MH3_TRACE`: game state over game time

`MH3_TRACE=[input][,monsters][,actions][,particles][,loading][,effect-objects][,net][,threads]`

The debug port turns each item on and off while the game runs (`trace ITEM on|off`, [debug port](debug-port.md)).

| Item | Effect |
| --- | --- |
| `input` | once per tick of game time: the sticks and game keys as the game read them, the field camera's yaw and pitch, player 0's position and facing, steps and key repeats since the last line ([variable step](optimizations/simulation/variable-step.md)) |
| `monsters` | once a second, each monster's move work, its in-use word and its position (`MH3 monsters` lines; field_camera.cpp) |
| `actions` | every action a monster starts (`MH3 monster action`: the work, its kind, the action before and after as mode.sub, the pair asked for, how and the caller, marked `control` when the monster control started it, the step counter and the step's scale), once per kind its species handlers (`MH3 monster handlers`), and each (kind, variant) whose enemy data EnemyActionTable looks up, once ([monster control](monster-control.md)) |
| `particles` | every third tick of game time, an `MH3 particles` line per particle manager with live particles (its emitter resource, the count, their mean age and life in ticks, the summed alpha of the first colour) and one with the emitter emissions, child curves (each run or gated off between ticks) and queued child emissions since the last; and an `MH3 effect emitter` line for every emitter the game looks up by name (each effect it creates: the name, the emitter data and the return addresses of five frames up). Compares effects at fixed and variable steps (`MH3_VARIABLE_STEP=0`) |
| `loading` | each loading screen: every file the loader queues as it is first seen, and every drawn frame an `MH3 loading` line with the loader state, the queued files and bytes, the disc bytes read, in flight and learned, the conversions done and queued, and the progress bar's share ([loading progress](optimizations/assets/load-time-asset-conversion.md)) |
| `net` | the network devices' requests, sockets, lookups and TLS sessions ([online](online.md)) |
| `threads` | every guest thread switch ([online](online.md#guest-threads)) |
| `effect-objects` | each effect object update callback and kind the first time it runs, and every 900 ticks (30 s of game time) each kind's mean life in ticks (`MH3 effect object lives`, field_lists.cpp) |

### `MH3_MONSTER_CONTROL`: drive a monster

`MH3_MONSTER_CONTROL=PATH`: commands read from PATH while the game runs (`act MODE SUB`, `every TICKS MODE SUB`, `ai off`, `freeze on`, `face DEGREES`, `goto X Y Z`, `target N`, `wait TICKS`, `list`), one a line, each run once in order as it appears. With `--quest 9004 --quest-monster NAME` a chosen monster in the arena pit, `--quest 9000 --quest-small NAME` four small ones ([monster control](monster-control.md)).

### `MH3_DEBUG_PORT`: drive the game over TCP

`MH3_DEBUG_PORT=PORT` (127.0.0.1) or `ADDR:PORT`, Linux: a TCP line protocol for memory peeks and pokes, guest calls, cheats and frame dumps while the game runs, whose commands skip the menus (`scripts/dev/debug-port.py`, [debug port](debug-port.md)).


### `MH3_EFFECT_TOUCH_CHECK`: the guest worker's join points

`MH3_EFFECT_TOUCH_CHECK=1` (Linux) checks that the game thread touches the
effect pools only after it has joined the guest worker's effect calculation
([guest worker](optimizations/threading/guest-worker.md)); an access before
the join is a race with the calculation. Each function that makes one is
reported once, and the pools open for the rest of that pass
(`effect_step.cpp`); development builds only.
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
(create MARKER when the part to trace starts).

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
| `AURORA_PORT_CAPTURE_DRAW_TIMING` | set: each captured draw's GPU finish time (glFinish after it), on the captured frames only | all |

## Read by tools, not the runtime

- `MH3_NV_DEBUG=FUNCTION`: the translator prints which instruction rejects
  each register in its nonvolatile-GPR analysis of that function.
- `MH3_CR_EXIT_AUDIT=1`: the translator lists every guest call after which
  the caller reads a volatile flag (CR0, CR1, CR5-CR7, XER.CA) before writing
  it, with the first reader (`[cr-exit-audit]` lines). The flag elision takes
  the EABI's word that none does; so far the only hits are `mfcr` copies of
  the whole CR.
- The build scripts' own variables (`MH3_ARCH`, `MH3_BUILD_DIR`,
  `MH3_EXTRA_CMAKE_ARGS`, `BUILD_JOBS`, `AURORA_ROOT`, `MH3_DEVELOPMENT` and
  `MH3_VITA_BUILD_DIR` for build-vita.sh, `PSP2CGC`, `MH3_VITA_ELF_CREATE`,
  `TRANSLATE_JOBS`, `OVERLAY_JOBS`, `DOTNET_BIN`) are described at the top of
  each script.
