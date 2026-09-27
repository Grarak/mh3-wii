# Local Aurora dependency

`aurora-main/` is a source snapshot copied from the sibling WiiCompiled checkout
on 2026-09-27 (parent commit `85f250155f8f397c1c463dc51dd4dc2707371d06`).
It preserves Aurora's MIT license at `aurora-main/LICENSE` and the same layout
used by WiiCompiled. The sibling checkout was not modified.

`scripts/build-aurora.sh` defaults to this source tree. `AURORA_ROOT` can still
select an explicit alternative. MH3-specific renderer patches remain in
`scripts/aurora-efb.cmake` and the Python overlay scripts, applied to build-local
copies. Aurora's original source is kept intact.

Common libraries (Dawn, SDL, Abseil, fmt, PNG, FreeType, zlib, ImGui, SQLite,
xxHash, zstd and Tracy) are **not committed here**. Aurora's existing CMake
providers find installed packages or fetch pinned versions into the build tree,
as in WiiCompiled. A first build may require network access. The target is
arm64 Linux: Dawn comes from the pinned prebuilt `linux-aarch64` package
(`v20260603.191052`), which includes the Vulkan backend MH3 requires. See
[arm64 Linux and Vulkan](linux-arm64.md).

The translator and its notices are already included under `toolchain/`.
`runtime/assets/dsp/dsp_coef.bin` is the free Dolphin DSP coefficient table,
with provenance in `toolchain/THIRD-PARTY-NOTICES.md`. The preparation script
verifies its SHA-256 before copying it to the ignored runtime asset directory;
it no longer needs files from the sibling WiiCompiled checkout.

From the repository root, with compiler/SDK, CMake, Ninja, Python and prepared
ROM-derived assets available:

```sh
./scripts/build-aurora.sh
ctest --test-dir build/linux-arm64 --output-on-failure
./scripts/run.sh --village
```

See the root README for translator and game-asset preparation. ROMs, extracted
game assets, generated translation, save data and development tools remain
outside version control.
