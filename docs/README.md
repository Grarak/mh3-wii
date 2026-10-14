# Documentation

## How the port works

[optimizations/](optimizations/README.md): every change this port makes
beyond a plain static recompilation, grouped by area (recompilation, CPU,
threading, rendering, assets, system, simulation, platforms), each with the
original game's behaviour, the replacement, the measurements and how to apply
it to another Wii game.

## Reference

- [vita-controls.md](vita-controls.md): the Classic Controller mapping on the
  Vita, gamepads and the keyboard.
- [online.md](online.md): online play through MH3SP and a local server for
  testing.
- [rom-decoder.md](rom-decoder.md): the native ISO/RVZ/WIA decoder.
- [cheats.md](cheats.md): the cheat list as native code.
- [variable-step-sites.md](variable-step-sites.md): every place the game's
  fixed 30 Hz logic was adapted to a variable step.
- [aurora-dependency.md](aurora-dependency.md): the Aurora copy in
  `aurora-main/`.
- [web-build.md](web-build.md): the WebAssembly/WebGPU build.

## Development

- [flags.md](flags.md): every environment variable the runtime reads.
- [profiling.md](profiling.md): statistics lines, profiling on the Vita and
  ARM boards, and what to measure.
- [debug-port.md](debug-port.md): driving a running Linux build over TCP.
- [monster-control.md](monster-control.md): spawning and driving any monster
  in the arena.
- [native-gx-original-calls.json](native-gx-original-calls.json): the game's
  GX call sites and their native replacements.
