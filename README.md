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

1. Install the kernel plugins [kubridge](https://github.com/bythos14/kubridge/releases)
   (0.3.1 or newer) and [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker/releases)
   under `*KERNEL` in `ux0:tai/config.txt` (or `ur0:tai/config.txt`), then
   reboot:

   ```
   *KERNEL
   ur0:tai/kubridge.skprx
   ur0:tai/CapUnlocker.skprx
   ```
   or
   ```
   *KERNEL
   ux0:tai/kubridge.skprx
   ux0:tai/CapUnlocker.skprx
   ```

2. Install `mh3-vita.vpk` with VitaShell.
3. Copy the disc image to `ux0:/data/mh3-wii/`.
   Name of the file doesn't matter, as long it has the correct file extension (iso, rvz)
   Make sure your ROM file is not corrupted, and that you have enough storage space left after putting the ROM there.

The first start checks the image and writes about 7 MB of disc metadata to
`ux0:/data/mh3-wii/disc/`; textures and models are converted while the game
loads them and cached there. Saves live in `ux0:/data/mh3-wii/saves/`. The
launcher's menus start the game, quests and custom quests, and set cheats,
widescreen, upscaling, MSAA and the frame limit
([controls](docs/vita-controls.md)). Online play uses the community
[MH3SP](https://github.com/sepalani/MH3SP) servers ([online](docs/online.md)).

The VPK holds the translated code and nothing of the disc: the game's
executable sections (`sys/main.dol`, checked by hash), textures, models,
sound and movies are read from your image.

## Building

Tested on arm64 Linux. Every target needs:

- Python 3, CMake 3.25+, Ninja and Git;
- the [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0) for the
  translator, in `tools/dotnet/` or named by `DOTNET_BIN`;
- [nodtool](https://github.com/encounter/nod/releases) to extract the disc
  for the translator (the commands below expect it in `tools/`).

The PS Vita and Linux builds also need **clang-21** with lld-21 and llvm-21
(`clang-21`, `clang++-21`, `llvm-ar-21` on `PATH`; the Vita build compiles
with them as well, not with VitaSDK's gcc), and the PS Vita and arm32 builds
need Docker.

For the PS Vita: [VitaSDK](https://vitasdk.org) (`VITASDK`, default
`/usr/local/vitasdk`) and **a copy of Sony's `psp2cgc.exe`** shader compiler
from the official PS Vita SDK's host tools, which cannot be distributed here.
Export its absolute path:

```sh
export PSP2CGC=/absolute/path/to/psp2cgc.exe
```

The build runs psp2cgc under Wine in Docker; the image is built from
[docker/psp2cgc-wine.Dockerfile](docker/psp2cgc-wine.Dockerfile) on first use
(on an arm64 host through qemu-user binfmt for i386). Every shader is
compiled once at build time into the executable.

For the web (WebAssembly):

- the [Emscripten SDK](https://emscripten.org) 6.0.11 in `~/emsdk` (or
  named by `EMSDK`):

  ```sh
  git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
  ~/emsdk/emsdk install 6.0.11 && ~/emsdk/emsdk activate 6.0.11
  ```

- glslangValidator, zstd and [naga](https://github.com/gfx-rs/wgpu/tree/trunk/naga),
  which convert the shaders to WebGPU's WGSL, a C++ compiler for the build
  machine, and Liberation Sans or DejaVu Sans, the launcher's font:

  ```sh
  sudo apt install glslang-tools zstd fonts-liberation clang
  cargo install naga-cli
  ```

### 1. Extract the disc

```sh
tools/nodtool extract -q 'Monster Hunter Tri (Europe) (En,Fr,De,Es,It).rvz' Assets/DATA
```

The build compiles the translator with dotnet and translates the game into
`generated/` by itself (the first build takes about 5 minutes longer), and
again, step by step, after a change of `symbols.txt`, `recomp.yml`, a site
list, the translator or a native port's registration or registers.
`generated/` and `Assets/` are not part
of the repository; `scripts/translate.sh` translates outside a build.

### 2. Build

The Linux and web scripts print one line when they are done and write the
compiler's output to `logs/` (`aurora-build.log`, `web-build.log`); read it
when a script stops without that line.

PS Vita (`build/vita/mh3-vita.vpk`):

```sh
scripts/build-vita.sh                        # release: ThinLTO and the PGO profile
MH3_DEVELOPMENT=ON scripts/build-vita.sh     # development: statistics and debug tools
```

Release builds use the checked-in profile `pgo/mh3-release.profdata.tar.zst`
([PGO and ThinLTO](docs/optimizations/cpu/pgo-and-thinlto.md)).

Linux, arm64 (development host, `build/linux-arm64-dev/MH3`):

```sh
MH3_ARCH=arm64 MH3_BUILD_DIR=build/linux-arm64-dev \
  MH3_EXTRA_CMAKE_ARGS='-DMH3_DEVELOPMENT=ON -DMH3_BUILD_VITA_TEXTURE_PACKER=OFF' scripts/build-aurora.sh
```

Linux, arm32 (armhf, Cortex-A9 code; for a Raspberry Pi or another ARMv7+
board; `build/linux-arm32/MH3`), cross-compiled in a Docker image built from
[docker/armhf-cross.Dockerfile](docker/armhf-cross.Dockerfile) on first use:

```sh
MH3_EXTRA_CMAKE_ARGS='-DMH3_BUILD_VITA_TEXTURE_PACKER=OFF' scripts/docker-build.sh scripts/build-aurora.sh
```

Like the Vita's, Linux builds are release builds unless `-DMH3_DEVELOPMENT=ON`
is added.

Web (WebAssembly and WebGPU, `build/web`):

```sh
scripts/build-web.sh                      # release: ThinLTO, build/web
MH3_DEVELOPMENT=ON scripts/build-web.sh   # development: statistics and debug tools, build/web-dev
scripts/serve-web.py build/web            # play it at http://localhost:8080/
```

The site is six files in `build/web`: `index.html`, `mh3-web.js`,
`isolation.js`, `MH3.js`, `MH3.wasm` and `MH3.data` (the launcher's font).
The first build also converts every shader to WGSL, which later builds take
from `~/.cache/mh3-web-shader-bank`. A release page always opens the launcher; a
development page also takes the command line from its address
(`?args=--quest+19001+--area+1&env=MH3_STATS=1`), which
`scripts/web-check.mjs` uses to test the web build in headless Chromium.

## Playing in a browser

`scripts/serve-web.py build/web` serves the page at `http://localhost:8080/`;
browsers run it only from `localhost` or over HTTPS. Any static HTTPS host
works too (GitHub Pages: publish those six files): the page then adds the
cross-origin isolation headers through a service worker and reloads once.
Open it in Chrome or Edge 113 or newer (Firefox and Safari where WebGPU is
turned on), choose your disc image and press Start. The page copies the
image into the browser's storage once (keep its size free on disk: 2.4 GB
for the RVZ), and keeps it, your saves and the settings between visits. The setup panel imports a save
(Dolphin's `Wii/title/00010000/524d4850` folder, or `saves` from Linux or the
Vita) and sets the display and mouse options and a frame counter
(<kbd>F2</kbd> during play). Start opens the Vita launcher's menus (the game,
quests, custom quests, cheats, SSAA, the frame limit) for keyboard, mouse or
gamepad: everything but online play.

## Running on Linux

```sh
MH3 --rom 'Monster Hunter Tri (Europe) (En,Fr,De,Es,It).rvz' --village   # first start
MH3 --village                   # file 1, character 1, straight into the village
MH3 --list quests               # also online-quests, arena-quests
MH3 --quest 19001 --area 1      # a quest, here Alatreon
MH3 --launcher                  # development builds: the Vita launcher's menus
```

The image is read in place; pass `--rom` again after moving it. Saves go to
`saves/` (`MH3_USER_DIR`). Keyboard and mouse follow Monster Hunter World's
defaults (WASD moves, a click captures the mouse for the camera, left and
right buttons attack, Q held with a click scrolls the item bar, Space evades:
[controls](docs/vita-controls.md)); SDL gamepads work too.

## License

The translator and the code derived from WiiCompiled are GPLv3 (`LICENSE`,
`toolchain/ORIGIN.md`, `toolchain/THIRD-PARTY-NOTICES.md`). Aurora is MIT
(`aurora-main/LICENSE`).
