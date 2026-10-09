# Monster Hunter Tri for the PS Vita

A static recompilation of Monster Hunter Tri (Wii) to native ARM code. The
game's PowerPC code is translated to C++ and linked with a native runtime
that replaces the Wii's hardware and system software: there is no emulator,
interpreter or JIT. Rendering goes through native GXM on the PS Vita and
OpenGL ES 3.0 on Linux; a WebAssembly build draws through WebGPU.

The repository contains no game code or data. You need your own copy of the
**European** disc (`RMHP08`, "Monster Hunter Tri (Europe) (En,Fr,De,Es,It)")
as an **ISO or RVZ** image. WIA images are recognized too, but untested and
possibly unstable. Other regions are not supported.

How the port reaches full speed on a 444 MHz Cortex-A9, written so that the
techniques carry over to other Wii games:
[docs/optimizations](docs/optimizations/README.md).

## Installing on the PS Vita

1. A Vita or PS TV with HENkaku/Ensō (taiHEN).
2. Install the kernel plugins [kubridge](https://github.com/bythos14/kubridge/releases)
   (0.3.1 or newer) and [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker)
   under `*KERNEL` in `ux0:tai/config.txt` (or `ur0:tai/config.txt`), then
   reboot:

   ```
   *KERNEL
   ur0:tai/kubridge.skprx
   ur0:tai/CapUnlocker.skprx
   ```

3. Install `mh3-vita.vpk` with VitaShell.
4. Copy the disc image to `ux0:/data/mh3-wii/`.

The first start checks the image and writes about 7 MB of disc metadata to
`ux0:/data/mh3-wii/disc/`; textures and models are converted while the game
loads them and cached there. Saves live in `ux0:/data/mh3-wii/saves/`. The
launcher's menus start the game, quests and custom quests, and set cheats,
widescreen, upscaling, MSAA and the frame limit ([launcher](docs/vita-launcher.md),
[controls](docs/vita-controls.md)). Online play uses the community
[MH3SP](https://github.com/sepalani/MH3SP) servers ([online](docs/online.md)).

The VPK holds the translated code and nothing of the disc: the game's
executable sections (`sys/main.dol`, checked by hash), textures, models,
sound and movies are read from your image.

## Building

Tested on arm64 Linux. You need:

- Python 3, CMake 3.25+, Ninja, Git and Docker;
- **clang-21** with lld-21 and llvm-21 (`clang-21`, `clang++-21`,
  `llvm-ar-21` on `PATH`; the Vita build compiles with them as well, not with
  VitaSDK's gcc);
- the [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0) for the
  translator, in `tools/dotnet/` or named by `DOTNET_BIN`;
- [nodtool](https://github.com/encounter/nod/releases) to extract the disc
  for the translator;
- for the Vita: [VitaSDK](https://vitasdk.org) (`VITASDK`, default
  `/usr/local/vitasdk`) and **a copy of Sony's `psp2cgc.exe`** shader
  compiler from the official PS Vita SDK's host tools, which cannot be
  distributed here. Export its absolute path:

  ```sh
  export PSP2CGC=/absolute/path/to/psp2cgc.exe
  ```

  The build runs psp2cgc under Wine in Docker; the image is built from
  [docker/psp2cgc-wine.Dockerfile](docker/psp2cgc-wine.Dockerfile) on first
  use (on an arm64 host through qemu-user binfmt for i386). Every shader is
  compiled once at build time into the executable.

### 1. Translate the game

```sh
tools/nodtool extract -q 'Monster Hunter Tri (Europe) (En,Fr,De,Es,It).rvz' Assets/DATA
tools/dotnet/dotnet build -c Release toolchain/translator/src/Translator.Cli
scripts/translate.sh            # writes generated/ (a few minutes)
```

`generated/` and `Assets/` are not part of the repository. Translate again
after changing `symbols.txt`, `recomp.yml`, `runtime/src/tick_sites.inc` or
adding or removing a native function registration (`REGISTER_NATIVE_FUNCTION_AS`).

### 2. Build

PS Vita (`build/vita/mh3-vita.vpk`):

```sh
scripts/build-vita.sh                        # release: ThinLTO and the PGO profile
MH3_DEVELOPMENT=ON scripts/build-vita.sh     # development: statistics and debug tools
```

Release builds use the checked-in profile `pgo/mh3-release.profdata.tar.zst`
([PGO and ThinLTO](docs/optimizations/cpu/pgo-and-thinlto.md)).

Linux, arm64 (development host):

```sh
MH3_ARCH=arm64 MH3_BUILD_DIR=build/linux-arm64-dev MH3_EXTRA_CMAKE_ARGS=-DMH3_DEVELOPMENT=ON \
  scripts/build-aurora.sh
```

Linux, arm32 (armhf, Cortex-A9 code; for a Raspberry Pi or another ARMv7+
board), cross-compiled in a Docker image built from
[docker/armhf-cross.Dockerfile](docker/armhf-cross.Dockerfile) on first use:

```sh
MH3_EXTRA_CMAKE_ARGS='-DMH3_BUILD_VITA_TEXTURE_PACKER=OFF' scripts/docker-build.sh scripts/build-aurora.sh
```

Like the Vita's, Linux builds are release builds unless `-DMH3_DEVELOPMENT=ON`
is added.

Web: `scripts/build-web.sh`, then `scripts/serve-web.py build/web`
([web build](docs/web-build.md)).

## Running on Linux

```sh
MH3 --rom 'Monster Hunter Tri (Europe) (En,Fr,De,Es,It).rvz' --village   # first start
MH3 --village                   # file 1, character 1, straight into the village
MH3 --list quests               # also online-quests, arena-quests
MH3 --quest 19001 --area 1      # a quest, here Alatreon
MH3 --launcher                  # development builds: the Vita launcher's menus
```

The image is read in place; pass `--rom` again after moving it. Saves go to
`saves/` (`MH3_USER_DIR`). Keyboard: Space = A, Left Shift = B, WASD = left
stick, IJKL = right stick, arrows = D-pad, Enter = Plus; SDL gamepads work too.
Runtime options: [flags](docs/flags.md); debugging and profiling:
[docs](docs/README.md).

## License

The translator and the code derived from WiiCompiled are GPLv3 (`LICENSE`,
`toolchain/ORIGIN.md`, `toolchain/THIRD-PARTY-NOTICES.md`). Aurora is MIT
(`aurora-main/LICENSE`).
