# Profile-guided optimization and ThinLTO

## What the original does

Nothing comparable: the game shipped as one CodeWarrior-compiled
executable. The recompiled game is generated C++ (about 11 million lines in
23,000 files) plus the runtime, and the compiler sees each translated
function with no idea which branches are hot.

## What the port does instead

Release builds (`MH3_DEVELOPMENT=OFF`) of the Vita and Linux targets use:

- **ThinLTO.** The game, the runtime, Aurora and the fetched dependencies
  compile to LLVM bitcode, and lld optimizes across all of them at link time
  (`-flto=thin`, `--lto-O2`, a ThinLTO cache in the build directory). This
  needs clang and lld; a release configure without them fails.
- **A checked-in profile.** `pgo/mh3-release.profdata.tar.zst` is an indexed
  clang source-based profile. A release configure with clang 21 or newer
  extracts it into the build directory and compiles with
  `-fprofile-instr-use`; the configure log says
  `MH3: release profile <path>`. `-DMH3_PGO_RELEASE_PROFILE=OFF` turns it
  off, `-DMH3_PGO_USE=FILE` uses another profile.

Source-based profiles match functions by name and a hash of their source
structure, not by target, and local functions are named by their file's
basename. A profile collected on arm32 Linux therefore applies to the
Vita build's shared code (the translated game, the runtime's ports and
Aurora's common code) from any checkout path. Code only one target compiles
(the GLES or GXM renderer, the Vita platform layer) has no record and keeps
the plain optimization. Development and release builds compile different
code, so a release build takes a profile from a release build.

## Why

Vita, Alatreon fight, development builds with and without an Alatreon
profile (2026-10-05; plain five runs, PGO three):

| | Plain | PGO | Change |
| --- | ---: | ---: | ---: |
| Logical fps | 57.4 | 65.1 | +13% |
| Displayed fps | 53.7 | 55.6 | +1.9 |
| Game thread logic (ms/frame) | 11.79 | 10.49 | −11% |
| Game thread draw section (ms/frame) | 3.83 | 3.34 | −13% |
| Render thread (ms/packet) | 14.40 | 13.17 | −8.5% |

The displayed rate was held by the GPU at the time, so the saved CPU time
showed as headroom.

## Collecting a new profile

The profile must come from the same source as the build that uses it;
records whose source changed are ignored without a warning, so refresh it
after larger changes.

```sh
# The armhf profile runtime, once per compiler version (compiler-rt).
scripts/docker-build.sh ./scripts/build-pgo-runtime.sh

# The instrumented arm32 release build.
MH3_BUILD_DIR=build/linux-arm32-pgogen-rel scripts/docker-build.sh env \
  MH3_EXTRA_CMAKE_ARGS="-DMH3_DEVELOPMENT=OFF -DMH3_PGO_GENERATE=ON -DMH3_PGO_RUNTIME=$PWD/build/pgo/libclang_rt.profile-armhf.a" \
  ./scripts/build-aurora.sh

# On an ARMv7 or ARMv8 Linux machine: one profile per run.
LLVM_PROFILE_FILE=out/run-%c%p.profraw ./MH3 --quest 19001 --area 1 ...

# Every run weighted the same, then packed for the repository.
llvm-profdata-21 merge -o mh3-release.profdata out/*.profraw
tar --owner=0 --group=0 --mtime=@0 -cf - mh3-release.profdata | zstd -19 > pgo/mh3-release.profdata.tar.zst
```

`MH3_PGO_GENERATE` instruments in continuous mode (`-fprofile-continuous`),
so a run that is killed still leaves its profile. Scripted input
(`MH3_INPUT`) and `--quest-monster` are in every build for these runs.

The checked-in profile covers every map (a quest on it, an area other than
the base camp, the hunter running in circles while the camera turns, 5 s),
every large monster the arenas hold (in the pit, waiting for it to attack,
20 s), Alatreon after its intro, Jhen Mohran and Ceadeus (20 s each). Every
run has weight 1, so a 20 s run counts for more than a 5 s one.

## Applying it to another Wii game

Generic: all of it. A recompiled game is a very large amount of generated
code with flat, branchy functions, which is what PGO and cross-module
inlining help most. Collect on a machine that runs the same CPU architecture
as the target when the target itself cannot write files easily; the
source-based profile carries over.

Game-specific: the runs. Cover every kind of scene a player spends time in
(each map, each boss, menus), not only the benchmark: code a profile never
saw is optimized for size.
