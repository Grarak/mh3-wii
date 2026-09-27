# Static RSO bring-up

The first dynamically loaded module, `01/hbm_data.rso`, is now included in the
static C++ translation. This is a fixed-address binding for the verified RMHP08
file, not yet a general RSO implementation. Remaining modules, alternate load
addresses, and unload/reload lifetimes still need bindings.

The original guest loader reads and links the module at `0x808196E0`. Its text
starts at `0x808197D0`, spans `0x2AC28` bytes, and its prolog is `0x8081A8A4`.
`scripts/link_rso.py` independently applies 3,449 internal and 1,188 external
relocations. Every initialized section was compared byte for byte against a
snapshot taken after the original linker finished. No snapshot is needed for a
normal build or run.

The format is documented by [Dolphin's RSO declarations](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/Debugger/RSO.h).
The RMHP08 loader at `0x804DB0A4` supplies the relocation behavior, and its SEL
initializer at `0x804DA544` supplies section addresses, including BSS. The script
checks the main DOL hash before applying those addresses. It rejects unsupported
relocations and unresolved imports rather than leaving them as zero.

`scripts/prepare_rso_translation.py` builds an analysis-only DOL with the linked
RSO sections and discovers function seeds from exports, standard prologues, and
constructor/destructor tables. The runtime data initializer still comes from
the original DOL; the module is not copied into guest memory at startup. The
module's native prolog wrapper verifies its base and linked text checksum before
entering compiled code, then enables executable-write protection for its text.

To repeat the comparison using the retained pre-constructor snapshot:

```sh
python3 scripts/link_rso.py Assets/DATA/files/01/hbm_data.rso \
  --base 0x808196e0 --output build/hbm-linked.bin \
  --verify-mem1 build/rso-boot-mem1.bin
python3 scripts/test_rso_linker.py
```

`MH3_MISSING_TARGET_MEM1_DUMP=path` writes MEM1 only when dispatch reaches a missing target.
Comparison requires a snapshot before constructors mutate initialized data;
a later boot boundary may no longer satisfy that condition. Local linked images,
analysis DOLs, generated code and snapshots stay under ignored build directories.

The Metal boot now executes the prolog and constructors and returns to game
initialization. Its next explicit stop is locked-cache address `0xE0000000`.
The translator emits 20,050 functions; this remains incomplete game coverage.
