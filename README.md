# Monster Hunter Tri static recompilation for macOS

**Work in progress: the native ARM64 executable builds and begins executing MH3,
but it is not playable yet.** The current boot reaches Wii interrupt initialization
and stops on an unimplemented MMIO write to `0xCC003004`.

This port uses WiiCompiled's PowerPC-to-C++ static translator and ISA/dispatch
support. There is no CPU interpreter or JIT fallback. The original sibling
WiiCompiled checkout is unchanged. The derived source is under GPLv3; see
`LICENSE`, `toolchain/ORIGIN.md`, and `toolchain/THIRD-PARTY-NOTICES.md`.

## Current verified results

- European disc ID `RMHP08`, extracted from the supplied RVZ.
- DOL SHA-256 `db5c9939f677c33d15d446aa4ad19c28044d0b80c5b3cc75adc7f1340ecab162`.
- Entry `0x80006310`, r13 `0x8079B2E0`, r2 `0x8079FF60`, verified from boot instructions.
- 19,244 statically translated functions; translator reports zero unsupported
  instructions and zero invalid SSA functions. This is not proof of full-game coverage.
- Function discovery uses 758 executable SEL exports, 72 verified compiler
  save/restore thunks, and speculative standard stack-frame starts.
- `build/native/MH3` is a linked macOS ARM64 executable.
- Runtime self-test passes RAM aliasing, big-endian access, translated stack stores,
  unmapped MMIO rejection, and MH3 exception-table initialization.
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
ctest --test-dir build/native --output-on-failure
```

Run the current boot implementation (expected to stop at an unimplemented service):

```sh
build/native/MH3 > logs/run.log 2>&1
```

The translation script propagates errors; unsupported instructions are not silently
stubbed. `WIICOMPILED_ROOT` defaults to `toolchain`, `DOTNET_BIN` to the local SDK,
and `BUILD_JOBS` to 6. Game outputs and logs remain local.

Extraction was performed with nodtool `v2.0.0-alpha.10`, macOS ARM64:

```sh
tools/nodtool extract -q 'Monster Hunter Tri (Europe) (En,Fr,De,Es,It).rvz' Assets/DATA
```

Its SHA-256 is `e23ca466999b720c55e6d29c9683fce8cc74451ba64ead2e543d50129f24528a`.
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
run reaches `__OSInterruptInit` at `0x804D1390`, whose instruction at `0x804D13E0`
writes the Wii interrupt controller at `0xCC003004`. This needs a native replacement
that preserves the guest interrupt table and mask state, followed by further
OS/hardware-service bring-up.

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
MH3 executable has no renderer linked yet.

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
