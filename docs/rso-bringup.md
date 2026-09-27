# Static RSO bring-up

Five boot modules are included in static C++ translation with fixed-address
bindings for the verified RMHP08 files. Alternate addresses, other modules, and
unload/reload lifetimes still require explicit support.

| Module | Base | Prolog |
| --- | --- | --- |
| hbm_data | `0x808196E0` | `0x8081A8A4` |
| com_data | `0x80868E40` | `0x8086907C` |
| lobby_data | `0x808C76A0` | `0x808C7ED8` |
| demo_data | `0x8097D080` | `0x80992FC8` |
| quest_data | `0x809D3DC0` | `0x809D60C4` |

Every initialized section of each offline-linked image matched a snapshot after
the original guest linker finished. The first module used `rso-boot-mem1.bin`;
the remaining four used `rso-next-mem1.bin`, captured at the com prolog before
constructors executed. Each source file and the original DOL are hash-pinned.
No snapshot is needed for a normal build or run.

The format is documented by [Dolphin's RSO declarations](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/Debugger/RSO.h).
The RMHP08 loader at `0x804DB0A4` supplies the relocation behavior, and its SEL
initializer at `0x804DA544` supplies section addresses, including BSS. The script
checks the main DOL hash before applying those addresses. It rejects unsupported
relocations and unresolved imports rather than leaving them as zero.

`scripts/prepare_rso_translation.py` builds an analysis-only DOL with the linked
RSO sections and discovers function seeds from exports, standard prologues, and
constructor/destructor tables. The five executable sections share bytes with one encompassing analysis data
span, keeping within the DOL section limit. The runtime data initializer still comes from
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

The Metal boot verifies and executes all five compiled prologs, prints media
version `1002121503`, and passes the former disc-padding boundary at partition byte
`968250889` with the preserved partition image. Both native and Metal runtime/audio tests pass. Translation emits
20,639 functions; this remains incomplete game coverage. Evidence is in
`logs/boot-rsos-run.log`.
