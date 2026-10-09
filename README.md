# Monster Hunter Tri static recompilation for ARM Linux and Vita

Monster Hunter Tri's PowerPC code (European disc `RMHP08`) is statically
recompiled to C++ and linked with a native runtime into one executable. There
is no interpreter or JIT fallback. Aurora provides **OpenGL ES 3.0** on Linux
and native **GXM** on Vita.

- **Targets:** Vita (ARM32, Cortex-A9, GXM), arm32 Linux (armhf, Cortex-A9
  build tested on Pi 5), and arm64 Linux for development.
- **Closeout, 2026-10-02:** game and rendering share one CPU thread;
  legacy GX/FIFO routes remain fatal. Release quest selection, direct native C
  material groups and processed GLES textures pass focused tests and a bounded
  host quest. Pi measurements distinguish owner CPU from the original GX
  workers. The owned scene provider remains unregistered and the render worker
  is not integrated. Full quest parity, retail memory peaks and 60 completed
  display FPS on Vita remain open. Busy GPU slots skip drawing while logic continues.
  [Native renderer status](docs/native-renderer-status.md) distinguishes current
  implementation, pending gates and earlier playable evidence.

## Build

Requirements: an arm64 Linux host with Python 3, CMake, Ninja, Clang 21,
Rust (`cargo`, for the disc image decoder), OpenGL ES/EGL development files and
Docker (for arm32), plus a .NET 8 SDK unpacked in `tools/dotnet` and nodtool
`v2.0.0-alpha.10`
([release](https://github.com/encounter/nod/releases/tag/v2.0.0-alpha.10);
the `linux-aarch64` build) in `tools/nodtool`. You need your own copy of the
European disc (`RMHP08`) as an RVZ, WIA or ISO. ROMs, extracted assets,
generated code, builds, saves and logs are all git-ignored.

### 1. Translate the PowerPC code

The translator reads the game's executable and overlays from an extracted
disc:

```sh
tools/nodtool extract -q 'Monster Hunter Tri (Europe) (En,Fr,De,Es,It).rvz' Assets/DATA
tools/dotnet/dotnet build -c Release toolchain/translator/src/Translator.Cli
scripts/translate.sh            # about 3.5 min; regenerates generated/
```

Rebuild the translator after changes under `toolchain/`. Retranslate after
adding or removing a native registration (`REGISTER_NATIVE_FUNCTION_AS`) or
editing `symbols.txt`, `recomp.yml` or `runtime/src/tick_sites.inc`; editing
the body of an existing native port only needs a rebuild.

### 2. Build the executable

arm64 (development host, `build/linux-arm64/MH3`):

```sh
MH3_ARCH=arm64 MH3_EXTRA_CMAKE_ARGS=-DMH3_DEVELOPMENT=ON scripts/build-aurora.sh
cmake --build build/linux-arm64 -j$(nproc) --target MH3     # later rebuilds
```

arm32 (the target: armhf, Cortex-A9 code generation, `build/linux-arm32/MH3`),
cross-compiled in the `mh3-armhf-cross` image, which `docker-build.sh`
builds on first use:

```sh
MH3_EXTRA_CMAKE_ARGS='-DMH3_DEVELOPMENT=ON -DMH3_BUILD_VITA_TEXTURE_PACKER=OFF' \
  scripts/docker-build.sh scripts/build-aurora.sh
scripts/docker-build.sh cmake --build build/linux-arm32 -j$(nproc) --target MH3   # later rebuilds
```

The container mounts the checkout at the same absolute path because the
translated data blobs use absolute `.incbin` paths; configure and build arm32
only inside it. `MH3_DEVELOPMENT` adds statistics, captures, scripted input
and port checks; leave it OFF for a plain build. Assertions follow
Release/NDEBUG independently. Configure and build logs go to `logs/aurora-*.log`.

Build options: `-DMH3_ROM_DECODER=OFF` builds without Rust (only a decrypted
ISO from `nodtool convert --decrypt` can then be read).
`MH3_BUILD_VITA_TEXTURE_PACKER` (ON by default) builds the offline texture
packer tool and needs upstream `stb_dxt.h` (`/usr/include/stb`,
`../Vita3K/external/stb` or `STB_INCLUDE_DIR`).

## Run

Pass the disc image once. The game reads it in place: the first launch only
writes the disc metadata (about 7 MB) to `disc/` next to the executable and
records where the image is, which takes well under a second. Textures and
models are converted while the game loads them, on three worker threads, and
kept in `disc/cache/`, so each one is converted only once. Every launch checks
that the image is still there and unchanged; pass `--rom` again after moving
it:

```sh
build/linux-arm64/MH3 --rom 'Monster Hunter Tri (Europe) (En,Fr,De,Es,It).rvz' --village
build/linux-arm64/MH3 --village                 # File 1, character 1, straight into the village
build/linux-arm64/MH3 --list quests             # also online-quests, arena-quests
build/linux-arm64/MH3 --quest 19001 --area 1 --no-damage    # Alatreon, the benchmark quest
```

`--quest <id>|free [--area <n>] [--spawn x,y,z] [--no-damage]` starts a quest
directly. `MH3_DISC_DIR` points at another prepared data directory instead.
Saves default to `saves/` (`MH3_USER_DIR` changes the root); `MH3_STATS=1`
prints game speed and fps periodically. On a VM without a working GPU driver
use `DISPLAY=:0 LIBGL_ALWAYS_SOFTWARE=1` (llvmpipe; virgl draws the geometry
wrong).

On the Vita, put the disc image in `ux0:/data/mh3-wii/`; the launcher writes
the same metadata and cache to `ux0:/data/mh3-wii/disc/`
([details](docs/vita-launcher.md#load-time-texture-and-model-conversion)).

For development runs, `scripts/dev/run-heartbeat.sh` launches with an isolated
copy of the post-Chief save fixture, dumps a frame every 150 game frames and
prints a heartbeat every minute:

```sh
MH3_ARGS="--quest 19001 --area 1 --no-damage" scripts/dev/run-heartbeat.sh alat 150
```

On the Raspberry Pi 5 (64-bit Raspberry Pi OS with armhf multiarch), copy
`build/linux-arm32/MH3` over and run it the same way, with `--rom` on the
first launch. `scripts/pi/benchmark.py` measures a quest with isolated saves
and scripted movement (`--binary MH3 --revision R --name N --quest 19001
--area 1`). It expects the save fixture at `/tmp/post-chief-data00.bin`
(`cat fixtures/post-chief-data00.bin | ssh PI 'cat > /tmp/post-chief-data00.bin'`).

Keyboard: Space = A, Left Shift = B, WASD = left stick, arrows = D-pad,
Enter = Plus ([controls](docs/controller-bringup.md)). Unported or unsupported
rendering paths fail explicitly rather than falling back.

[HANDOVER.md](HANDOVER.md) covers debugging tools, environment variables,
tests and conventions; the `docs/` notes are listed in
[docs/README.md](docs/README.md).

## Verified facts

- DOL SHA-256 `db5c9939f677c33d15d446aa4ad19c28044d0b80c5b3cc75adc7f1340ecab162`.
- Entry point `0x80006310`, r13 `0x8079B2E0`, r2 `0x8079FF60`.
- Over 20,000 translated functions in the DOL, plus the 15 RSO overlay
  profiles (village, quest, enemy and map00–map11 fields). The translator
  reports no unsupported instructions.

## License

The port uses WiiCompiled's PowerPC-to-C++ translator and ISA/dispatch support.
The derived source is under GPLv3; see `LICENSE`, `toolchain/ORIGIN.md` and
`toolchain/THIRD-PARTY-NOTICES.md`. Aurora is MIT (`aurora-main/LICENSE`).
