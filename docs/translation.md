# Translation in the build

The translated code in `generated/` comes from the translator
(`toolchain/translator`, C#) run over the disc's DOL and RSO modules. CMake
runs `scripts/translate.py` at every configure, before it reads the shard
list, and lists the translation's inputs as configure dependencies: after an
edit of one of them, `ninja` (or a build script) configures again and the
driver brings `generated/` up to date first. Nothing has to be translated by
hand; `scripts/translate.sh` runs the same driver outside a build (`--force`
runs every step).

## Steps

Each step records a digest of its inputs in `generated/translation-state.json`
and runs only when that digest changes or an output is missing. A failed or
interrupted step runs again next time. Its full output goes to `logs/`.

| Step | Inputs | Outputs | Time |
| --- | --- | --- | --- |
| translator | the C# and project files of `Translator.Cli`, `Translator.Core`, `WiiCompiled.Setup.Common` | `bin/Release/net8.0/*.dll` (`dotnet build`) | 5–30 s |
| native index | the translator, every source under `runtime/src` | `generated/native_index.json` | 1 s |
| prepare | the translator, the DOL, `mh3.sel`, the RSOs, `symbols.txt`, `recomp.yml`, `tick_sites.inc`, the preparation scripts | `build/MAP.txt`, the analysis DOL, the data initializer, `rso_binding.h` | a few s |
| translate | the translator, the native index, the prepare outputs, the site lists | `generated/functions`, `translation.json` | about 35 s |
| overlay *profile* (18) | the translator, the native index, the DOL, the profile's RSO, `symbols.txt`, `recomp.yml`, the site lists | `generated/rso_overlays/` | 40–120 s each, `OVERLAY_JOBS` (5) at a time: 3.5 min for all |
| shards | the native index, the translator, `translation.json`, the generated functions | `generated/build_shards/` (`prepare_native_dispatch.py`) | 4 s, 1 s when fresh |

The translator's outputs are content-addressed or rewritten only when they
change, so ninja recompiles only the shards whose functions changed.

## The native index

The translator reads the runtime's C++ for its native ports: the
registrations (`REGISTER_NATIVE_FUNCTION(_AS)`, `PPC_NATIVE_OVERRIDE`,
`GX_FATAL_STUB`), the void stubs' ABIs and each port's guest effects (the
registers it reads and writes, whether it can suspend). The
`emit-native-index` command writes exactly that, plus the C linkage of the
precise natives the shards call directly, as one JSON file, rewritten only
when it changes. The translation and the shards take the runtime's sources
only through it: an edit of a runtime source costs a configure (about 2 s)
and the index (1 s) besides its compile, about 5 s with the link on the arm64
host, and translates again only when a port is added, removed or reads or
writes other registers. Then the DOL and every overlay translate again (about
4 minutes), since any translated caller may depend on the port's contract.
With nothing changed the driver takes about 1.5 s.

## Requirements

- The extracted disc in `Assets/DATA` (`tools/nodtool extract`).
- The .NET 8 SDK in `tools/dotnet/` or named by `DOTNET_BIN`. The arm32
  Docker build runs the same dotnet from the checkout; a translator change is
  best built on the host first, since the container has no NuGet cache.
- Concurrent configures of several build directories share `generated/`:
  the driver holds `generated/.translation.lock` while it runs.
