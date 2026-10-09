# Monster Hunter Tri static recompilation for macOS

**Work in progress: the native ARM64 executable builds, plays the opening movie,
and accepts input on the title menu, but gameplay is not verified yet.** Aurora
renders through native Metal; MoltenVK is not required.

Verified boot behavior includes OS/IOS55 and DVD initialization, five statically
translated RSO modules, the Nintendo wrist-strap screen, the Capcom logo, and THP
movie playback. The runtime creates and reopens the NAND system file, six
character-save slots, network data, and save banner. See [NAND bring-up](docs/nand-bringup.md)
and [RSO bindings](docs/rso-bringup.md).

Classic Controller reports drive movie skipping and title-menu navigation.
Selecting Begin the Game passes HID and keyboard initialization and renders all
six save slots. File selection and the main menu work. Selecting New Game now
completes its CPU framebuffer read/modify/write loop, then reaches an
the animated character scene. Display lists, indexed matrix loads, and bounded
vertex uploads execute. The scene still has repeated smaller images and missing
UI; frame/copy sequencing is under investigation.
See [framebuffer support](docs/efb-color-bringup.md). See [controls](docs/controller-bringup.md),
[HID startup](docs/usb-hid-bringup.md), and [function discovery](docs/function-discovery.md).
The SDL gamepad source passes a virtual-device mapping/hotplug test; live game
input validation so far uses the diagnostic controller file. Native PCM mixing and DMA tests pass, and
real boot opens SDL audio and executes AX callbacks; audible output remains
unverified.

This port uses WiiCompiled's PowerPC-to-C++ static translator and ISA/dispatch
support. There is no CPU interpreter or JIT fallback. The original sibling
WiiCompiled checkout is unchanged. The derived source is under GPLv3; see
`LICENSE`, `toolchain/ORIGIN.md`, and `toolchain/THIRD-PARTY-NOTICES.md`.

## Current verified results

- European disc ID `RMHP08`, extracted from the supplied RVZ.
- DOL SHA-256 `db5c9939f677c33d15d446aa4ad19c28044d0b80c5b3cc75adc7f1340ecab162`.
- Entry `0x80006310`, r13 `0x8079B2E0`, r2 `0x8079FF60`, verified from boot instructions.
- Over 20,000 statically translated functions; translator reports zero unsupported
  instructions and zero invalid SSA functions. This is not proof of full-game coverage.
- Function discovery uses 758 executable SEL exports, 72 verified compiler
  save/restore thunks, and speculative stack-frame/indirect-function discovery.
- `build/native/MH3` is a linked macOS ARM64 executable.
- Runtime self-test passes RAM aliasing, big-endian access, translated stack stores,
  unmapped MMIO rejection, MH3 exception/interrupt-table initialization, device
  state, and SRAM persistence/checksums.
- A synthetic translator regression passes for full native replacements, wrappers
  that require original code, and ordinary callees.

## Reproduce with the installed project-local tools

The local SDK is .NET `8.0.425`; CMake, Ninja and Clang are already available.
The SDK and extracted content are deliberately ignored by Git.

Build the MH3-local translator:

```sh
DOTNET_CLI_HOME="$PWD/tools/dotnet-home" NUGET_PACKAGES="$PWD/tools/nuget" \
  tools/dotnet/dotnet build \
  toolchain/translator/src/Translator.Cli/Translator.Cli.csproj \
  -c Release -p:NuGetAudit=false
```

Translate, generate the build graph, compile, and test:

```sh
./scripts/translate.sh
./scripts/build.sh
python3 scripts/test_native_discovery.py
python3 scripts/test_disc_reader.py
ctest --test-dir build/native --output-on-failure
```

Run the current boot implementation (expected to stop at an unimplemented service):

```sh
MH3_TRACE_ERRORS=1 build/native/MH3 > logs/run.log 2>&1
```

The translation script propagates errors; unsupported instructions are not silently
stubbed. `WIICOMPILED_ROOT` defaults to `toolchain`, `DOTNET_BIN` to the local SDK,
and `BUILD_JOBS` to 6. Game outputs and logs remain local.

Extraction was performed with nodtool `v2.0.0-alpha.10`, macOS ARM64:

```sh
tools/nodtool extract -q 'Monster Hunter Tri (Europe) (En,Fr,De,Es,It).rvz' Assets/DATA
tools/nodtool convert --decrypt 'Monster Hunter Tri (Europe) (En,Fr,De,Es,It).rvz' build/disc-decrypted.iso
python3 scripts/prepare_partition.py build/disc-decrypted.iso
```

The preserved `Assets/DATA/partition.bin` retains exact padding bytes for aligned
reads beyond file ends. Preparation strips sector hash blocks from the decrypted
ISO and verifies the boot header and FST against extraction. It uses about 4 GiB;
the intermediate ISO can be removed after preparation. Both remain ignored.

The nodtool executable SHA-256 is `e23ca466999b720c55e6d29c9683fce8cc74451ba64ead2e543d50129f24528a`.
Release: https://github.com/encounter/nod/releases/tag/v2.0.0-alpha.10

## Runtime progress and remaining work

The original translation failure was a branch to low-memory debugger code at
`0x60`, reached through exception-vector setup. `runtime/src/os_boot.cpp` now
replaces MH3's `OSExceptionInit` at `0x804CB5A0`, preserving its handler-table
state while using host exceptions. The translator now stops discovery at full
native replacements; `REGISTER_NATIVE_FUNCTION_AS` wrappers still discover
original code. See `scripts/test_native_discovery.py`.

The first host crash was a translated stack store into an unmapped fixed host
window. MH3's checked-memory backend now covers those stores too. The subsequent
run reached `__OSInterruptInit` at `0x804D1390`. That routine and hardware mask
programming at `0x804D1460` now have native replacements preserving the guest
interrupt table, masks and external exception handler. Native interrupt delivery
from host services still requires scheduler integration.

EXI and SI initialization now retain guest queues and interrupt bindings. Absent
GameCube expansion devices report absence. The SRAM host service maintains a
64-byte settings image in `saves/sram.bin` (override the directory with
`MH3_USER_DIR`), with checksum and lock/interrupt semantics. Tests use a temporary
directory and never modify real saved settings.

The translated default thread-switch callback is explicitly seeded in
`symbols.txt`. Hardware-only DSP bootstrap and MI/BAT setup are replaced at their
OS boundaries; the latter preserves interrupt bindings and the original
shutdown-list registration. This does not implement audio playback or scheduling.

The console byte writer sends the original C library's formatted output to the
host log. Boot metadata includes MEM1/MEM2 arenas and IOS55, requested by the
supplied disc's TMD. IOS55 metadata follows
[Dolphin's extracted IOS values](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/IOS/VersionInfo.cpp).

With `MH3_TRACE_ERRORS=1`, runtime failures emit symbolized native backtraces
before unwinding, so routine diagnosis needs no LLDB authorization.

Boot now passes IPC, STM, NAND, system configuration, DVD initialization and
C++ constructors. The latest trace enters game main (`0x8003F278`), passes
the original `VIInit` (`0x804E7B00`), and reaches GX initialization. The Metal build now consumes guest GX setup commands and converts vertex-array
addresses; it binds eight initial textures, completes display copies, and delivers retrace callbacks. The next boundary is RSO static translation.
The DOL constructor/destructor tables are explicitly seeded for static translation,
including constructor stubs with no ordinary stack prologue.
IOS synchronous open/close/read/write use a private NAND root at
`$MH3_USER_DIR/nand` (default `saves/nand`), preserving file modes and offsets.
Opening does not create files. Path traversal and symlink following are rejected.
`/dev/es` exposes the TMD-verified title ID and data directory. `/dev/fs` can be
opened, but its filesystem-control operations are not implemented yet.

Asynchronous file operations enqueue results. A wrapper around the original
`SCCheckStatus` pumps a bounded batch of callbacks while preserving CPU state;
this lets the SDK load saved configuration or apply its existing defaults when
files are absent. General scheduler event delivery remains to be connected.
WiiConnect24 request/time devices explicitly report unavailable, allowing the
SDK's offline path to continue. Unknown devices still fail explicitly.

STM event subscriptions remain pending; host events write the guest output and
queue callback records. LED/dimming settings are retained, and event-hook release
queues its completion. Tests cover file persistence, modes, missing files, path
isolation, ES identity, asynchronous error delivery, callback CPU preservation,
and STM subscriptions/events. STM protocol behavior was checked against
[Dolphin's STM implementation](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/IOS/STM/STM.cpp).

`DiscImage` maps the extracted partition's system files and FST file extents
back to their original byte offsets. It validates metadata, directory bounds,
file lengths and overlapping extents. Gaps use exact bytes from the optional
`partition.bin`, whose size and metadata are checked; missing padding remains an
explicit error. Tests cover hash-block stripping, nonzero gap bytes, cross-file
reads, partition bounds/mismatches, and exact bytes from 13 real assets.
`MH3_DISC_DIR` selects the extracted partition root (default `Assets/DATA`).

The IOS disc service supports inquiry, disc ID, partition reads, cover/status
queries and asynchronous completion records. Delivery outside the configuration
poll remains part of scheduler integration. It does not emulate arbitrary raw
physical-disc reads. ES ticket views preserve the supplied ticket metadata;
the fresh host NAND reports no prior consumption records. Protocol references:
[Dolphin DI](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/IOS/DI/DI.h),
[ES formats](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/IOS/ES/Formats.h).

Remaining requirements for a playable port include:

- OS scheduling, interrupts, timing, DVD access and other hardware services.
- MH3-specific GX, audio and input bindings, with validated guest layouts.
- Loading and statically translating 21 RSO modules, including relocations,
  symbol resolution, module lifecycle and native dispatch.
- Aurora integration and actual on-screen/gameplay verification.

The checked runtime deliberately raises errors for unimplemented MMIO/GX access
and executable modifications. It does not claim those operations succeeded.

Aurora already supports native Metal in the supplied WiiCompiled checkout
(`aurora-main/lib/dawn/MetalBinding.mm`), so MoltenVK is unnecessary. The current
headless executable remains available at `build/native/MH3`.
`./scripts/build-aurora.sh` builds an Aurora-linked executable at `build/metal/MH3`,
using the sibling source (override with `AURORA_ROOT`). Dependency sources are
cached under `build/aurora/_deps`. The local CMake hook fixes SDK header ordering
and standard-header visibility without changing the sibling checkout.

`build/metal/MH3 --graphics-check` initializes native Metal, opens a 640×480
window, submits 30 frames, and exits. This checks the host renderer path only;
the guest GX bridge is partial and actual game frames are still incomplete. Renderer caches
are kept under `saves/graphics`. On this Apple M2 Max, the check selected Metal,
created a BGRA8Unorm presentation surface, submitted all 30 frames, and exited
successfully. The Aurora-linked executable also passes the runtime self-test.
A sandbox that denies macOS display access must allow the graphics run to access
the desktop; a denied display is not a Metal backend failure.

## Tools and evidence

- `scripts/inspect_disc.py`: writes `build/disc-inventory.json`, including 919
  static SEL exports and all 21 RSO import/export/relocation inventories.
- `scripts/generate_symbols.py`: writes `build/MAP.txt`; validates the input hash
  and every compiler-thunk instruction. Only executable SEL exports become function seeds.
- `tools/python/bin/python scripts/disassemble.py ADDRESS [BYTE_COUNT]`: PPC
  disassembly using project-local Capstone 5.0.9.
- `logs/translator-build.log`, `logs/translate.log`, `logs/shards.log`,
  `logs/native-build.log`, `logs/run.log`: current build/run evidence.

ROMs, extracted assets, generated game code/data, build products, SDKs, and logs
are ignored by `.gitignore` and must remain out of source commits.

The scoped host VI interface retains register state and supplies beam positions;
the original VI timing and framebuffer setup still runs. Analog encoder writes
are retained by a native I2C boundary. Retrace delivery is connected at VIWaitForRetrace boundaries; general
preemptive interrupt delivery and scanout selection remain incomplete. Static jump-table discovery follows a scaled selector back to its
logical register, fixing the VI format switch; all nine jump-table tests pass.

The GX bridge frames fragmented CP/BP/XF commands and forwards complete packets
in guest byte order. Vertex-array addresses become Aurora host-pointer commands,
bounded to the containing MEM1/MEM2 region. Both ordinary and 64-bit FIFO writes
use this path. Non-paletted texture addresses are converted with tiled/mipmap footprint checks.
Palette/TLUT handling, drawing, display lists, scaled/texture copies, and
asynchronous GPU completion events remain incomplete. Unscaled display copies
and the blocking GXDrawDone path are connected.
Scoped CP/PE/PI registers preserve the original guest FIFO initialization state;
this is not a complete hardware model. Tests cover fragmented command framing,
array conversion, rejection of unconverted pointers, and register aliases/masks.

The latest Metal boot returns from GX initialization, consumes 298 commands,
converts 16 vertex-array bindings, and binds eight textures. Supported image
formats are I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR. The bridge preserves masked BP
register updates and checks every tiled mip level against accessible guest RAM.
Tests include block rounding, mip-chain bounds, masked writes, metadata byte
order, and rejection of indexed formats pending palette integration.

Display copies validate framebuffer stride and the accessible RAM extent before
calling Aurora's GPU presentation-copy path. They do not populate guest XFB RAM.
Blocking `GXDrawDone` flushes guest dirty state, submits the Aurora frame, waits
for the renderer worker and Dawn queue completion, then signals the SDK flag,
callback, and wait queue while preserving caller CPU state. The latest run logged
successful Metal completion and was sampled in `VIWaitForRetrace` (`0x804E8050`)
through the guest scheduler. The spinning test process was then stopped.

`VIWaitForRetrace` now paces the host to PAL/NTSC field timing and invokes MH3's
original VI handler with a latched beam position. That preserves pending register
updates and game callbacks. The boundary also services host window events and
queued IOS completions. SI sampling configuration stays in the original SDK
state, and asynchronous STM presentation controls enqueue completions.

The boot FST is installed in reserved high MEM1, excluded from the advertised
arenas. DVD command-status polling now pumps IOS completions without reentering
the callback pump. MH3 successfully reads `mh3.sel` and `01/hbm_data.rso`, then
calls the latter's compiled prolog at `0x8081A8A4` (module base `0x808196E0`).
The offline linker matches the original loader byte for byte, and a runtime
check verifies linked text before dispatch. See [RSO bring-up](docs/rso-bringup.md)
for the fixed-address binding and its current limits.
