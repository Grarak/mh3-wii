# arm64 Linux and Vulkan

MH3 targets **arm64 (aarch64) Linux** and renders only through **Vulkan**. Aurora
draws through Dawn (a WebGPU implementation), and the build requires Dawn's
Vulkan backend. The runtime fails at startup if Aurora falls back to any other
backend. No other operating system, CPU architecture or graphics API is a
supported target.

The port was brought up on macOS ARM64 with Metal. Bring-up logs and
measurements in the other documents were recorded there unless they say
otherwise. Those results have not yet been reproduced on arm64 Linux.

## Status

Verified by cross-compiling for `aarch64-linux-gnu` (Clang 18, Ubuntu 24.04
arm64 multiarch libraries), without game assets:

- Aurora's CMake configure succeeds for arm64 with the pinned prebuilt Dawn
  package `v20260603.191052` (`dawn-linux-aarch64.tar.gz`). That package
  contains the Vulkan backend (it loads `libvulkan.so.1`). SDL3 configures
  X11, Wayland and KMSDRM video, Vulkan, ALSA and PulseAudio.
- Aurora, SDL3, the MH3 source overlays (EFB preservation, viewport bias,
  async frame worker, shared render buffers), every runtime source except the
  two that include translator output, and `MH3ControllerHostTest` compile and
  link for aarch64.
- `main.cpp` and `abi_bridge.cpp` need `generated/RuntimeConfig.h`, and the
  executable needs the translated functions. Both come from running the
  translator on the game DOL, which was not available for this check.

Not yet verified on arm64 Linux: translation on a Linux host, linking `MH3`,
`--self-test`, `--graphics-check`, `--efb-check`, boot, gameplay, audio and
performance.

## Dependencies

Build machine (Debian/Ubuntu package names; install the `:arm64` variants of the
`-dev` packages when cross-compiling from another architecture):

```sh
sudo apt install clang cmake ninja-build python3 pkg-config dotnet-sdk-8.0 \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
  libxtst-dev libxfixes-dev libwayland-dev libxkbcommon-dev libdrm-dev \
  libgbm-dev libegl-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev
```

Run time: a Vulkan 1.1+ driver and `libvulkan1`, for example Mesa Turnip
(Adreno), PanVK (Mali) or V3DV (Raspberry Pi 4/5), or NVIDIA's L4T driver on
Jetson. Check with `vulkaninfo --summary` before running MH3.

Aurora fetches Abseil, fmt, xxHash, libpng, ImGui, SQLite, zstd, Tracy and SDL3
into `build/aurora-arm64/_deps` on the first configure. Several of these are
GitHub source archives. If the network blocks archive downloads, clone the
pinned tags and pass `-DFETCHCONTENT_SOURCE_DIR_<NAME>=<path>`.

## Building

```sh
./scripts/build-aurora.sh            # build/linux-arm64/MH3 (Vulkan)
./scripts/build.sh                   # build/linux-arm64-cpu/MH3 (no renderer)
./scripts/run.sh --village
```

`scripts/target.sh` detects the machine. On arm64 Linux the scripts build
natively. Anywhere else they pass `cmake/toolchains/linux-arm64.cmake`, which
cross-compiles with Clang for `aarch64-linux-gnu`. Set `MH3_SYSROOT` to an arm64
root filesystem, or install the arm64 multiarch `-dev` packages on the build
host. `MH3_BUILD_DIR` overrides the output directory.

`runtime/CMakeLists.txt` fails to configure for anything other than arm64 Linux.
`-DMH3_ALLOW_UNSUPPORTED_HOST=ON` lets other hosts configure for development
only. Such a build still needs a Dawn with the Vulkan backend, and CMake checks
for one.

Dawn comes from Aurora's pinned `linux-aarch64` package by default. To use a
locally built Dawn, set `AURORA_DAWN_PROVIDER=system DAWN_DIR=<install prefix>`
when running `build-aurora.sh`.

## Translating

Run `scripts/translate.sh` on Linux; `DOTNET_BIN=dotnet` selects a system SDK.
The translator writes `generated/data_sections_init_blobs.S` in the object
format of the machine it runs on: ELF sections and unprefixed symbols on Linux,
Mach-O on macOS. It also uses absolute `.incbin` paths. A `generated/` tree made
on macOS or in another checkout therefore cannot be reused.

## Runtime configuration

| Variable | Effect |
| --- | --- |
| `MH3_ASYNC_RENDER=0` | Encode and submit frames synchronously (A/B baseline). |
| `MH3_SHARED_RENDER_BUFFERS=1` | Bind Aurora's mapped staging ring directly as vertex/uniform/index/storage buffers. Opt-in and unvalidated on Vulkan; only takes effect when Dawn exposes `BufferMapExtendedUsages`. The log states whether it was enabled. |
| `MH3_TRACE_ERRORS=1` | Print symbolized native backtraces on runtime errors. |
| `MH3_USER_DIR`, `MH3_DISC_DIR` | Save/NAND root and extracted partition root. |

CMake options: `MH3_ENABLE_GPU_VALIDATION` keeps Dawn validation on in Release
builds. `MH3_ENABLE_GX_DIAGNOSTICS`, `MH3_ENABLE_GUEST_CALL_TRACE` and
`MH3_ENABLE_COLLISION_CHECK` are debug-only and default OFF.

## Platform notes

- **Frame worker.** The MH3 async overlay (`scripts/patch_aurora_async.py`)
  keeps frame preparation, sealing and presentation on the game's main thread.
  The bounded worker only encodes and submits sealed frames. That was the
  validated macOS model, and it now applies on Linux as well. Upstream Aurora's
  Linux path would otherwise prepare frames on the worker and present from a
  separate presenter thread.
- **Shared render buffers.** Dawn's Vulkan backend advertises
  `BufferMapExtendedUsages` only when a memory type is both device-local and
  host-visible, which is typical of ARM SoC GPUs with unified memory. The
  feature is requested whenever it is advertised, but the direct path stays
  opt-in.
- **NAND metadata** is stored in the `user.mh3.nand` extended attribute. The
  save directory must be on a filesystem with user xattrs (ext4, btrfs, xfs;
  tmpfs on Linux 6.6 and newer).
- **Backtraces.** `MH3` is linked with exported symbols so glibc's
  `backtrace_symbols` can name functions.
- **Dolphin reference runs.** `scripts/run-dolphin-reference.sh` uses
  `dolphin-emu` from `PATH` on Linux, or `DOLPHIN_BIN`.

## Open question: Dawn

Aurora's renderer is written against WebGPU, with about 865 `wgpu::` uses across
37 files and WGSL shaders. Dawn implements WebGPU on Vulkan and compiles WGSL to
SPIR-V (Tint). Removing Dawn means either porting Aurora and the MH3 overlays to
another WebGPU implementation such as wgpu-native, which would need replacements
for the Dawn-specific toggles, pipeline cache and `BufferMapExtendedUsages`, or
writing a native Vulkan backend for Aurora's GX renderer. Neither is planned
yet. The prebuilt arm64 Dawn package works today.
