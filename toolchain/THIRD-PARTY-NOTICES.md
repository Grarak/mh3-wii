# Third-Party Notices

WiiCompiled itself is licensed under the GNU General Public License v3.0
(see [`LICENSE`](LICENSE)). It incorporates, links against, or redistributes the third-party
components listed below. Each remains under its own license and copyright.

Nothing listed here is Nintendo intellectual property. This project ships no game code, assets,
or data of any kind - see the [README](README.md).

---

## Bundled in this repository

### aurora - MIT

Copyright (c) 2022 Luke Street.
Source: <https://github.com/encounter/aurora> - vendored in `aurora-main/`, license text at
`aurora-main/LICENSE`.

Aurora itself vendors:

- **magic_enum** 0.7.2 - MIT, Copyright (c) 2019-2021 Daniil Goncharov.
  `aurora-main/include/magic_enum.hpp`.
  Source: <https://github.com/Neargye/magic_enum>
- **libogc-derived SRAM structures** - zlib-style license, Copyright (c) Michael Wiedenbauer
  (shagkur) and Dave Murphy (WinterMute). `aurora-main/lib/card/SRAM.hpp`.
  Source: <https://github.com/devkitPro/libogc>

### Dolphin Emulator - reference

Dolphin was also used extensively as a behavioural reference during development of this project's
hardware and IOS high-level implementations.

### Dolphin Emulator Riivolution code - GPL-2.0-or-later

Copyright (c) 2021 Dolphin Emulator Project.
The runtime's Riivolution patch handling is a port of Dolphin's
`Source/Core/DiscIO/RiivolutionParser.{h,cpp}` and the external-path resolution rules of
`Source/Core/DiscIO/RiivolutionPatcher.cpp`, adapted in
`runtime/include/hle/riivolution_contract.h` and `runtime/src/hle/storage/riivolution.cpp`
(both marked `SPDX-License-Identifier: GPL-2.0-or-later`).
Source: <https://github.com/dolphin-emu/dolphin>

### pugixml - MIT

Copyright (c) 2006-2025 Arseny Kapoulkine.
The Riivolution XML reader uses pugixml 1.15, vendored in
`runtime/third_party/pugixml` from commit `ee86beb30e4973f5feffe3ce63bfa4fbadf72f38`.
Source and license: <https://github.com/zeux/pugixml>

### Crypto++ 8.9.0 - Boost Software License 1.0 / public domain

Copyright (c) 1995-2019 Wei Dai and contributors.
The runtime uses Crypto++ for SHA-1 and sect233r1 ECDSA key derivation and signing. Its portable
sources are vendored in `runtime/third_party/cryptopp`; assembly implementations are disabled.
Source: <https://github.com/weidai11/cryptopp/tree/CRYPTOPP_8_9_0>. Full license text:
`runtime/third_party/cryptopp/License.txt`.

### toml11 4.4.0 - MIT

Copyright (c) 2017 Toru Niina.
The runtime configuration reader and scalar string writer use the single-header distribution,
vendored in `runtime/third_party/toml11`.
Source: <https://github.com/ToruNiina/toml11/tree/v4.4.0>. Full license text:
`runtime/third_party/toml11/LICENSE`.

### YamlDotNet - MIT

Copyright (c) Antoine Aubry and contributors.
Referenced by `translator/src/Translator.Core`. Source: <https://github.com/aaubry/YamlDotNet>

### libco - ISC (valgrind.h: BSD-style)

Copyright byuu and the higan team.
Non-Windows builds use libco's symmetric stackful coroutines in place of Win32 Fibers for guest
OSThread scheduling (`runtime/src/fiber_manager.cpp`). Vendored in full (all non-Windows
CPU-architecture backends - amd64, x86, arm, aarch64, ppc, ppc64v2, plus the portable sjlj
fallback - though this project's x86_64-only target only ever compiles amd64.c) in
`runtime/third_party/libco` from commit `e18e09d634d612a01781168ad4d76be10a7e3bad`.
Source: <https://github.com/higan-emu/libco>. Full license text:
`runtime/third_party/libco/LICENSE`.

### libbzip2 1.0.8 - bzip2 license (BSD-style)

Copyright (C) 1996-2019 Julian R Seward.
The disc image decoder (`runtime/src/rom_decoder.cpp`) decompresses bzip2 RVZ/WIA groups with
the library sources of the official `bzip2-1.0.8.tar.gz` (SHA-256
`ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269`), unmodified, vendored in
`runtime/third_party/bzip2`. Source: <https://sourceware.org/bzip2/>. Full license text:
`runtime/third_party/bzip2/LICENSE`.

### LZMA SDK 25.01 decoders - public domain

Igor Pavlov. The disc image decoder's LZMA and LZMA2 decompression uses `LzmaDec` and `Lzma2Dec`
with their headers from the 7-Zip 25.01 source release (`7z2501-src.tar.xz`, SHA-256
`ed087f83ee789c1ea5f39c464c55a5c9d4008deb0efe900814f2df262b82c36e`), unmodified, vendored in
`runtime/third_party/lzma`. Each file states that it is in the public domain.
Source: <https://www.7-zip.org/sdk.html>

### compiler-builtins 0.1.160 memory helpers - MIT AND Apache-2.0 WITH LLVM-exception

Copyright (c) The Rust Project contributors. `runtime/vita/aeabi_memory.c` is a C port of the ARM
EABI memory helpers (`src/arm.rs`) and generic memory routines (`src/mem/impls.rs`) that the Vita
build previously linked from the nod decoder's Rust archive.
Source and license texts: <https://github.com/rust-lang/compiler-builtins> (`LICENSE.txt`).

### nod - MIT OR Apache-2.0 (reference)

Copyright (c) Luke Street. The disc image decoder reproduces the reads of nod 1.4.4's
`Disc`/partition streams for this project's bridge; no nod code is included in it, and the nod
bridge it replaced is no longer built.
Source: <https://github.com/encounter/nod>. The RVZ junk generator follows Dolphin's
`docs/WiaAndRvz.md` (CC0-1.0).

---

## Fetched at build time and redistributed in release builds

These are pinned in `aurora-main/extern/CMakeLists.txt`, `aurora-main/CMakeLists.txt`,
`aurora-main/cmake/AuroraDawnProvider.cmake`, and (for Mbed TLS) `runtime/CMakeLists.txt`. They are
not stored in this repository; the build downloads them - each fetch is pinned to an exact version
with a checked SHA-256 - and links or redistributes the resulting binaries. Their license texts are
included in the installer's `licenses/` folder. The Windows installer bundles the pinned source
trees themselves (fetched by `Launcher/Prepare-Dependencies.ps1`) so end-user builds run offline.

| Component | Version | License | Upstream |
| --- | --- | --- | --- |
| Mbed TLS | 3.6.7 | Apache-2.0 / GPL-2.0-or-later | <https://github.com/Mbed-TLS/mbedtls> |
| Dawn (WebGPU) | `v20260603.191052` prebuilt | BSD-3-Clause | <https://dawn.googlesource.com/dawn> |
| Tint (part of Dawn) | with Dawn | BSD-3-Clause | <https://dawn.googlesource.com/dawn> |
| DirectXShaderCompiler (`dxcompiler.dll`) | with Dawn | NCSA / University of Illinois Open Source | <https://github.com/microsoft/DirectXShaderCompiler> |
| SDL | 3.4.4 | zlib | <https://github.com/libsdl-org/SDL> |
| libusb (linked into SDL on Windows) | 1.0.30 | LGPL-2.1-or-later | <https://github.com/libusb/libusb> |
| Dear ImGui | 1.91.9b-docking | MIT | <https://github.com/ocornut/imgui> |
| {fmt} | 11.1.4 | MIT | <https://github.com/fmtlib/fmt> |
| xxHash | 0.8.3 | BSD-2-Clause | <https://github.com/Cyan4973/xxHash> |
| zlib | 1.3.2 | zlib | <https://github.com/madler/zlib> |
| libpng | 1.6.58 | PNG Reference Library License v2 | <https://github.com/pnggroup/libpng> |
| FreeType | 2.14.3 | **FreeType License (FTL)** - see below | <https://freetype.org/> |
| Zstandard | 1.5.7 | **BSD-3-Clause** - see below | <https://github.com/facebook/zstd> |
| SQLite | 3.51.3 amalgamation | Public domain | <https://sqlite.org/> |
| Tracy Profiler | pinned commit | BSD-3-Clause | <https://github.com/wolfpld/tracy> |
| C++/WinRT | - | MIT (Microsoft) | <https://github.com/microsoft/cppwinrt> |
| nodtool (disc image extraction) | v2.0.0-alpha.10 | MIT OR Apache-2.0 | <https://github.com/encounter/nod> |

### Dual-licensed components - elections made by this project

- **FreeType** is offered under the FreeType License (FTL) or GPL-2.0. **This project elects the
  FreeType License.** The FTL requires the following credit, which is given here and reproduced in
  distributed builds:

  > Portions of this software are copyright © 2026 The FreeType Project (www.freetype.org).
  > All rights reserved.

- **Zstandard** is offered under BSD-3-Clause or GPL-2.0. **This project elects BSD-3-Clause.**
  Copyright (c) Meta Platforms, Inc. and affiliates.

## Bundled in the setup executable's toolkit payload

The distributed `WiiCompiled-Setup.exe` carries a build toolkit so that translation and
compilation can run on a machine with nothing preinstalled. These tools are redistributed
unmodified, with their license texts, in the installer's `licenses/` folder.

| Component | License | Upstream |
| --- | --- | --- |
| llvm-mingw (Clang, LLD, libc++, libunwind, MinGW-w64 runtime) | Apache-2.0 with LLVM Exception; MinGW-w64 runtime under its own permissive terms; bundled GNU utilities under GPL-2.0-or-later or GPL-3.0-or-later | <https://github.com/mstorsjo/llvm-mingw> |
| CMake | BSD-3-Clause | <https://cmake.org/> |
| Ninja | Apache-2.0 | <https://ninja-build.org/> |
| nodtool (disc image extraction) | MIT OR Apache-2.0 | <https://github.com/encounter/nod> |
| Microsoft Visual C++ Runtime (`vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll`) | Microsoft redistributable terms | Microsoft Visual Studio |
| `dxil.dll` | Microsoft redistributable (proprietary signing library) | Microsoft |

> [!IMPORTANT]
> The GNU utilities bundled inside llvm-mingw are GPL-licensed. Their complete corresponding source
> is available from the upstream project linked above at its pinned version, and this project will
> supply it on request for the exact version shipped in any given release. Pins live in
> `Launcher/Prepare-PortableTools.ps1` and `Launcher/NativeBuildFlags.ps1`.

---

## Development-only dependencies

Not redistributed in any release artifact.

| Component | License |
| --- | --- |
| xUnit.net 2.4.2, xunit.runner.visualstudio 2.4.5 | Apache-2.0 |
| Microsoft.NET.Test.Sdk 17.6.0 | MIT |
| coverlet.collector 6.0.0 | MIT |
| .NET 8 SDK | MIT |

---

## Reference material

Not code, but the documentation this project depends on:

- [WiiBrew](https://wiibrew.org/wiki/) - Wii hardware and IOS documentation.
- [Custom Mario Kart Wiiki (Tockdom)](https://wiki.tockdom.com/) - Mario Kart Wii file formats and
  modding documentation.
- [Retro Rewind](https://wiki.tockdom.com/wiki/Retro_Rewind) by ZPL - the mod distribution this
  project can build as a static profile. No Retro Rewind content is redistributed here; users
  supply their own copy.

---

If you believe a component is missing or misattributed here, please open an issue.
