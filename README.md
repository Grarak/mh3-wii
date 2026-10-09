# Monster Hunter Tri static recompilation for ARM Linux and Vita

Monster Hunter Tri's PowerPC code (European disc `RMHP08`) is statically
recompiled to C++ and linked with a native runtime into one executable. There
is no interpreter or JIT fallback. Aurora provides **OpenGL ES 3.0** on Linux
and native **GXM** on Vita.

- **Targets:** Vita (ARM32, Cortex-A9, GXM), arm32 Linux (armhf, Cortex-A9
  build tested on Pi 5), and arm64 Linux for development.
- **Current migration, 2026-10-01:** game and rendering share one CPU thread;
  legacy GX/FIFO routes remain fatal. Native startup, UI/scene/effect families,
  dynamic materials and resident asset preparation have focused verification.
  GPU model transforms/texgen and live shape/material routing remain in progress;
  complete quest rendering and current FPS/RAM results are not established.
  Busy frame slots/display queues skip rendering while logic continues.
  [Native renderer status](docs/native-renderer-status.md) distinguishes current
  implementation, pending gates and earlier playable evidence.

**[HANDOVER.md](HANDOVER.md)** is the working guide: setup, translation, arm64
and arm32 builds, running on the VM and the Pi, debugging tools, conventions
and open work. The `docs/` notes are listed in [docs/README.md](docs/README.md).

## Quick start (arm64 migration build)

```sh
tools/dotnet/dotnet build -c Release toolchain/translator/src/Translator.Cli
scripts/translate.sh
MH3_ARCH=arm64 MH3_EXTRA_CMAKE_ARGS=-DMH3_DEVELOPMENT=ON scripts/build-aurora.sh
build/linux-arm64/MH3 --village
```

The launch is an integration probe: unported or unsupported rendering paths
fail explicitly. For arm32 Linux, build inside the `mh3-armhf-cross` Docker image
(`scripts/docker-build.sh scripts/build-aurora.sh`). See HANDOVER.md.

Keyboard: Space = A, Left Shift = B, WASD = left stick, arrows = D-pad,
Enter = Plus ([controls](docs/controller-bringup.md)).

## Game assets

Extract the European disc with nodtool `v2.0.0-alpha.10`
([release](https://github.com/encounter/nod/releases/tag/v2.0.0-alpha.10); use
the `linux-aarch64` build):

```sh
tools/nodtool extract -q 'Monster Hunter Tri (Europe) (En,Fr,De,Es,It).rvz' Assets/DATA
tools/nodtool convert --decrypt 'Monster Hunter Tri (Europe) (En,Fr,De,Es,It).rvz' build/disc-decrypted.iso
python3 scripts/prepare_partition.py build/disc-decrypted.iso
```

`Assets/DATA/partition.bin` keeps the exact padding bytes for aligned reads
past file ends. Preparation strips the sector hash blocks from the decrypted
ISO and checks the boot header and FST against the extraction. The
intermediate ISO can be deleted afterwards. ROMs, extracted assets, generated
code, builds, saves and logs are all git-ignored.

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
