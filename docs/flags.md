# Runtime flags

Every environment variable the runtime reads. Most exist only in development
builds (`MH3_DEVELOPMENT=ON`); the Builds column says which.

## Settings

| Variable | Effect | Builds |
| --- | --- | --- |
| `MH3_DISC_DIR` | game data directory (default `disc/` next to the executable; Vita `ux0:/data/mh3-wii`) | all |
| `MH3_USER_DIR` | save/NAND root (default `saves`; Vita `ux0:/data/mh3-wii/saves`) | all |
| `MH3_NATIVE_ASSET_DIR` | a whole-disc converted texture/model set instead of the load-time cache | all |
| `MH3_WIDESCREEN` | `0`: 4:3; default 16:9 ([widescreen](optimizations/rendering/widescreen.md)) | all |
| `MH3_FRAME_LIMIT` | `60` (default), `30` or `off`, as the launcher's `Framelimit` row; `off` for benchmarks | all |
| `MH3_MSAA` | `2`, `4` or `off`, as the launcher's `MSAA` row | development |
| `MH3_LANGUAGE` | `en`, `fr`, `de`, `it` or `es`, as the launcher's `Language` row | all |
| `MH3_STATS` | `0`: no once-per-second statistics lines ([profiling](profiling.md#statistics-lines)) | development |
| `MH3_WINDOW_SCALE` | `n`: an n times larger window | development |
| `MH3_RENDER_SCALE` | `n`: render the EFB at n times the native resolution; overrides `Upscaling` | development |
| `MH3_THP_JPEG` | `off`: decode movies in software on the Vita ([movies](optimizations/assets/hardware-movie-decoding.md)) | development |
| `MH3_AUDIO_NULL` | `1`: no playback device | development |
| `MH3_LAUNCHER_INPUT` | a launcher input script: lines `FRAME BUTTONS` (`up+cross`, `none`, ...) | development |
| `MH3_VARIABLE_STEP` | `0`: the game's fixed 1/30 s steps | development |
| `MH3_GXM_SHADER_DUMP` | a directory for runtime-compiled GXPs and their Cg | Vita development |

## Online

See [online](online.md) for each.

| Variable | Effect | Builds |
| --- | --- | --- |
| `MH3_ONLINE_SERVER` | `HOST[:PORT]`: the server (default `mh3sp.com`) | all |
| `MH3_ONLINE_SKIP_TLS` | `1`: no TLS (a local MH3SP with `UseSSL = OFF`) | all |
| `MH3_ONLINE_CA` | a DER root CA to pin instead of MH3SP's | all |
| `MH3_ONLINE_NAME_TAG` | text before the hunter's name in the lobby (default `[PV]` on the Vita) | all |
| `MH3_ONLINE_HOSTS` | `name=address[,...]`: answer these lookups without DNS | Linux |
| `MH3_ONLINE_PORTS` | `from=to[,...]`: port redirects | Linux |
| `MH3_ONLINE_ADDRESSES` | `from=to[,...]`: IPv4 address redirects | Linux |
| `MH3_ONLINE_AUTO` | `id=N,server=N,gate=N,city=N,post=QUEST[,...]`: an autopilot from `--city` into a quest (`net/online_auto.cpp`) | development |

## Development tools

Each takes a comma-separated list of items; unknown items are reported at
startup.

| Variable | Items | What it does |
| --- | --- | --- |
| `MH3_CAPTURE` | `PATH`, `interval=N`, `start=N`, `frame=N`, `audio=FILE`, `ram=N`, `ram-frames=N` | BMP frame dumps, raw audio, MEM1/MEM2 dumps |
| `MH3_PORT_CHECK` | `NAME`/`all`, `every`, `dump=DIR`, `originals`, `tolerance=R`, `P:`/`E:`/`C:`/`A:0xADDR`, `fuzz-after=N`, `fuzz-samples=N`, `fuzz-demo=`, `fuzz-frames=` | runs native ports against their translated originals and fuzzes them (`runtime/include/port_check.h`, [native ports](optimizations/cpu/native-function-ports.md)); runs single-threaded |
| `MH3_STEP_PROBE` | `T`, `step=N`, `settle`, `all` | dumps state that counts steps instead of time ([variable step](optimizations/simulation/variable-step.md)) |
| `MH3_PROFILE` | `split`, `render`, `sync`, `skip=KIND`, `fill`, `disc`, `census` | split timers and per-frame reports ([profiling](profiling.md)) |
| `MH3_INPUT` | `file=PATH`, `record=PATH`, `playback=PATH`, `camera=N[:D]` | scripted controller input ([controller input](optimizations/system/controller-input.md)) |
| `MH3_TRACE` | `input`, `monsters`, `actions`, `particles`, `loading`, `net`, `threads`, `demo`, `movie`, `rand`, `effect-objects` | game state lines over game time; the debug port toggles them |
| `MH3_MONSTER_CONTROL` | `PATH` | drives a monster from commands in a file ([monster control](monster-control.md)) |
| `MH3_DEBUG_PORT` | `PORT` or `ADDR:PORT` | a TCP protocol for peeks, pokes, guest calls and cheats, Linux ([debug port](debug-port.md)) |
| `MH3_EFFECT_TOUCH_CHECK` | `1` | reports game-thread touches of the effect pools before the guest worker's join, Linux ([guest worker](optimizations/threading/guest-worker.md)) |

## Build-option tools

These need a CMake option, which rebuilds the generated code.

| Variable | Option | What it does |
| --- | --- | --- |
| `MH3_WRITE_WATCH` | `-DMH3_WRITE_WATCH=ON` | `START,LENGTH` or `list=FILE`, `ms=N`: reports the guest code storing to memory |
| `MH3_CALL_TRACE` | `-DMH3_ENABLE_GUEST_CALL_TRACE=ON` | `PATH[,gate=MARKER]`: guest call edges as CSV (`scripts/trace_guest_calls.py`); with `-DMH3_ENABLE_FUNCTION_TRACE=ON`, `PATH,movie` traces host calls around movies (`scripts/function_trace.py`) |
| `MH3_ENTRY_TRACE` | `-DMH3_ENABLE_ENTRY_TRACE=ON` | `PATH[,gate=FILE]`: entry counts per translated function (`scripts/dev/entry_trace_compare.py`) |

## Asset overrides

| Variable | Effect | Builds |
| --- | --- | --- |
| `MH3_NATIVE_MODEL_INDEX` | a converted model index instead of `native-models/models.index.bin` | all |
| `MH3_NATIVE_MODEL_PACK` | one converted model pack instead of the index | all |

## Aurora

| Variable | Effect | Builds |
| --- | --- | --- |
| `AURORA_PORT_CAPTURE` | `DIR`: a per-draw state log and the shader sources | all |
| `AURORA_PORT_CAPTURE_DRAW_INTERVAL` | `N`: log every Nth draw (default 30) | all |
| `AURORA_PORT_CAPTURE_DRAW_TIMING` | set: each captured draw's GPU time | all |

## Read by tools, not the runtime

- `MH3_NV_DEBUG=FUNCTION`: the translator explains its nonvolatile-GPR
  analysis of that function.
- `MH3_CR_EXIT_AUDIT=1`: the translator lists guest calls after which the
  caller reads a volatile flag before writing it.
- The build scripts' variables (`MH3_ARCH`, `MH3_BUILD_DIR`,
  `MH3_EXTRA_CMAKE_ARGS`, `BUILD_JOBS`, `MH3_DEVELOPMENT`, `PSP2CGC`, ...) are
  described at the top of each script.
