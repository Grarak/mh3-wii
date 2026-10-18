# Native C ports of guest functions

## What the original does

Every function of the game's PowerPC executable (the DOL, about 20,000
functions) and of its relocatable modules is translated to C++ by the
WiiCompiled translator (`toolchain/translator`, output in `generated/`). A
translated function keeps the guest's view of the machine: it reads and
writes a `CpuContext` register file, loads and stores big-endian guest
memory, and calls other functions through the guest ABI. The result is
correct but generic code.

## What the port does instead

A function whose cost or behaviour matters is replaced by hand-written C or
C++ that does the same thing, registered at the guest address it replaces:

```cpp
extern "C" void func_80048890(CpuContext* ctx);          // the translated original
extern "C" void MH3_ClearDrawBuckets(CpuContext* ctx) { ... }
REGISTER_NATIVE_FUNCTION_AS(0x80048890, MH3_ClearDrawBuckets, "ClearDrawBuckets");
```

- The translator scans `runtime/src` for the literal addresses
  (`recomp.yml`, `native_registration_root`), so translated callers call the
  native directly and the generated dispatch tables resolve indirect calls to
  it (`scripts/prepare_native_dispatch.py`). The translated original is still
  compiled and stays callable for comparisons.
- A native that only touches constant-index registers (`ctx->gpr[3]`,
  `ctx->fpr[1]`) and passes register values, not `ctx`, to its helpers is
  *precise*: callers sync only the registers its contract lists and call it
  directly ([call boundary](../recompilation/translator-call-boundary.md)).
- Every port starts from the translated function's Ghidra decompilation
  (`scripts/dol2elf.py` builds a labelled ELF, `scripts/ghidra/Mh3Decompile.java`
  decompiles), and the function gets a name in `symbols.txt`. Ghidra is only
  needed for large, branchy functions; the translated C++ in `generated/`
  is often enough to port from.
- Development builds check a port against its translated original:
  `MH3_PORT_CHECK=Name` runs both from one RAM and register snapshot and
  reports every differing word (`runtime/include/port_check.h`). Rarely
  reached ports (player actions, enemy checks) are fuzzed with
  `P:0xADDR`/`E:0xADDR`/`C:0xADDR` items ([flags](../../flags.md)). Ports
  that ran clean for long enough drop their check.
- Ports may trade bit-exactness for speed where the difference is checked
  with a tolerance (the separate multiply-add of
  [Cortex-A9 floating point](../recompilation/cortex-a9-floating-point.md) is such a case);
  many are bit-exact, such as the NEON matrix rows of PSMTXConcat on arm32.

`runtime/src` holds about 1,650 registrations in 156 files. Groups and
where they are described:

| Group | Examples | Notes |
| --- | --- | --- |
| Matrix and vector math | PSMTXConcat/ConcatArray, PSMTXMultVec, PSMTXInverse, the game's matrix wrappers (0x80088428, 0x80071120, ...) | `runtime/src/mtx.cpp`, `msl_math.cpp` |
| NW4R g3d animation and models | AnmObjChrRes, FrameCtrl, G3dCalcWorld, G3dCalcSkinning, CalcViewLc, ResAnmChr evaluators | `runtime/src/g3d_calc_world.cpp`, `runtime/native/mh3_model_view.c` |
| NW4R effects | Effect::Calc 0x800A4C20, ParticleManager::Calc 0x800AC35C, the draw strategies | [particle batching](../rendering/particle-batching.md) |
| Sound engine, MIX, SEQ/SYN | 109 entry points | [native audio](../system/native-audio.md) |
| Everything that used GX | scene queue, UI, HUD, effects, scene feedback | [GX replacement](../rendering/gx-replacement.md) |
| Collision and land queries | ground candidate lists, the land sweep, hit spheres | [compact native data](compact-native-data.md) |
| Step-length dependent logic | shells, enemy turning, player actions, timers | [variable step](../simulation/variable-step.md) |
| Wii Remote pointer | the game's copy of KPAD (0x800450AC..0x800468A0), the pointer and cursor (0x80043C10..0x80044DDC), pointing at monsters for the log (0x802DB354) | `runtime/src/pad_pointer.cpp`, `log_pointer.cpp`, [controller input](../system/controller-input.md) |
| Cheats | wrappers that change arguments or results | [cheats](../../cheats.md) |

## Why

Translated code pays for the guest machine model on every instruction:
register-file traffic, byte swaps, dispatch guards. A native port lets the
host compiler keep values in registers and use NEON. Measured examples:

- Particle effects: with the effect ports and draw batching, the Pi 5 game
  thread (Cortex-A9 build, Alatreon, moving) went from 4.29-4.58 to
  4.04-4.08 ms per frame, effects from 1.35 to 1.00 ms.
- Skinning summed with NEON and fewer record copies: Vita draw time
  14.08 → 12.98 ms per frame in the Alatreon fight.
- Root motion evaluating only the root node in the native AnmObjChrRes
  ports: animation-curve evaluation fell from 4.0-4.3% to 2.6% of the Vita
  game thread.
- The HUD (about 2 ms a frame of the Alatreon fight on the Vita), the debug
  font's text measures (37.7% of the game thread on the online notice
  screen before the port) and the creature groups' passes (4.4% of the game
  thread) are native.

Ports are also how the game is changed where translation alone cannot: GX
calls ([GX replacement](../rendering/gx-replacement.md)) and per-step amounts
([variable step](../simulation/variable-step.md)).

## Example

`runtime/src/draw_buckets.cpp`, the whole port of ClearDrawBuckets
(0x80048890), which the task pass runs every frame:

```cpp
constexpr uint32_t kBuckets = 0x80796D30, kBuffer = 0x8065B3A0;
constexpr uint32_t kBucketBytes = 0x4880;

// The first 32 halfwords of each array are list heads: 0xFFFF is empty.
constexpr uint32_t kHeadBytes = 32 * 2;

// 0x80048890 ClearDrawBuckets: zero both arrays, then empty their heads.
void ClearDrawBuckets(CpuContext*) {
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t buckets = Memory::Read32(kBuckets + 4 * i);
        FillNativeGuestMemory(buckets, 0, kBucketBytes);
        std::memset(Memory::GetPointer(buckets, kHeadBytes), 0xFF, kHeadBytes);
    }
}

extern "C" void func_80048890(CpuContext* ctx);
extern "C" void MH3_ClearDrawBuckets(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    PortCheck::Call(ctx, "ClearDrawBuckets", func_80048890, ClearDrawBuckets);
#else
    ClearDrawBuckets(ctx);
#endif
}
REGISTER_NATIVE_FUNCTION_AS(0x80048890, MH3_ClearDrawBuckets, "ClearDrawBuckets");
```

The translated original cleared 2 × 0x4880 bytes with guest stores; the port
is two `memset`s. `FillNativeGuestMemory` also retires renderer resources
that lived in the cleared range.

## Applying it to another Wii game

Generic (comes with the WiiCompiled runtime and this repository's tools):
the registration macros (`runtime/include/abi_bridge.h`), the dispatch
emission, `PortCheck`, the labelled-ELF and Ghidra scripts, and the rule that
a port keeps its translated original for comparison.

Game-specific: which functions are hot, their addresses, and the structure
offsets they use. The order that worked here:

1. Profile on the slowest target (a sampling profiler on the Vita, or `perf`
   on an ARM board; [profiling](../../profiling.md)) and list the translated
   functions by self time.
2. Port leaves first (math, accessors), then their callers; a port that calls
   back into translated code still pays for the guest call boundary.
3. Keep ports precise (no `ctx` to helpers) where they are called often.
4. Compare every port with `PortCheck` at a fixed step before using it.
