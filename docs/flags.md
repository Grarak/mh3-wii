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

## Renamed and removed

A development build names any of these that is still set at startup.

| Was | Now |
| --- | --- |
| `MH3_CAPTURE_PATH`, `_INTERVAL`, `_START`, `_FRAME` | `MH3_CAPTURE=PATH,interval=,start=,frame=` |
| `MH3_AUDIO_DUMP=FILE` | `MH3_CAPTURE=audio=FILE` |
