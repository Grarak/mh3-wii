# Flat guest memory

## What the original does

The Wii's CPU sees its RAM through several address aliases, and hardware
next to it:

| Region | Guest addresses | Size |
| --- | --- | --- |
| MEM1 | `0x80000000` (cached), `0xC0000000` (uncached), `0x00000000` (physical) | 24 MiB |
| MEM2 | `0x90000000`, `0xD0000000`, `0x10000000` | 64 MiB |
| Locked L1 cache | `0xE0000000` | 16 KiB |
| EFB (framebuffer) | `0xC8000000` | |
| Hardware registers (VI, PE, PI, DSP, DI, AI, the GX FIFO, ...) | `0xCC000000`-`0xCDFFFFFF` | |

Game code mixes all of these: MH3's g3d model code computes matrices in the
locked cache and DMAs them out (`func_804C63D0` alone made 487 million
locked-cache accesses in the traced runs), the SDK writes the GX FIFO at
`0xCC008000` with 8-byte paired-single stores, and the VI and AI code polls
registers.

## What the port does instead

WiiCompiled's runtime looked each access up in a software MMU (one bias per
1 MiB page; pages without a bias went through `Read*Slow`/`Write*Slow`).
The port maps guest memory flat: `host = kGuestBase + addr`, plus a byte
swap (`runtime/include/guest_flat_memory.h`).

- **Linux:** one `PROT_NONE` reservation; MEM1, MEM2 and the locked cache
  are mapped into it from a shared-memory object (memfd) at each alias, so
  every alias shares the same pages. On arm32 the 4 GiB window wraps
  (`kGuestBase = 0x90000000`, guest `0x80000000` lands at host
  `0x10000000`).
- **Vita:** a process cannot reserve those addresses, so `Host()` folds the
  aliases first (MEM1 → `0xB0000000`, MEM2 → `0xC0000000`, locked cache →
  `0xB4000000`) into fixed views of one backing block reserved through the
  kubridge kernel plugin ([Vita platform](../platforms/vita-platform-layer.md)).
- **Web:** WebAssembly has no virtual memory, so the aliases fold as on the
  Vita, into linear memory below the static data (`-sGLOBAL_BASE`): MEM1 at
  16 MiB, the locked cache at 80 MiB, MEM2 at 272 MiB, ending at 336 MiB
  ([web build](../platforms/web-build.md)). Stray accesses are not reported there, as
  WebAssembly has no signals.
- **The locked cache is plain memory.** A locked-cache DMA (the DMAU/DMAL
  registers) is a synchronous `memcpy` (`TransferLockedCache`,
  `runtime/src/ppc_helpers.cpp`). g3d's lock request (0x80502DD0) is a native
  that always refuses, so ScnMdlSimple's world, skinning and view passes
  work in main memory instead of staging matrices in the cache and copying
  them out (`runtime/src/g3d_calc_world.cpp`).
- **Hardware stays unmapped.** The 129-line list
  `runtime/src/hardware_access.inc` names the translated functions that may
  touch hardware; the translator emits their loads and stores through
  `MemoryHw`, which forwards hardware addresses to the device models. Any
  other function that touches hardware faults, and the fault reporter names
  the guest address, the region and the translated function. The list came
  from a trace of every slow-path access over 71 runs plus a static scan.
- Most GX FIFO writers no longer exist: the game's GX callers are native
  ([GX replacement](../rendering/gx-replacement.md)) and the SDK's GX entry points stop
  the game.

## Why

Every translated load and store paid for the lookup. Before the flat
mapping, an intermediate fix that only inlined the page-bias check for
stack accesses noted that each stack store had been an out-of-line call,
16% of the game thread in the field (commit "Read and write plain RAM
inline under checked guest memory"). The flat mapping removed
the tables, the slow paths and the deferred-read pages altogether; its own
gain was not measured separately. It is also what lets a second host thread
run guest code ([guest worker](../threading/guest-worker.md)): there is no per-thread
memory state.

## Example

The whole access path on the Vita (`runtime/include/guest_flat_memory.h`):

```cpp
inline constexpr uintptr_t kGuestBase = 0xB000'0000u;

inline uint8_t* Host(uint32_t guestAddress) noexcept {
    const uint32_t offset = (guestAddress & 0x1fffffffu) | ((guestAddress >> 3) & 0x04000000u);
    return reinterpret_cast<uint8_t*>(kGuestBase + offset);
}
```

The mask folds `0x8xxxxxxx`/`0xCxxxxxxx` and `0x9xxxxxxx`/`0xDxxxxxxx` onto
one view each; the shifted bit moves the locked cache (`0xE0000000`) to
`0xB4000000`, distinct from the EFB range.

An entry of `runtime/src/hardware_access.inc`:

```text
0x80047DD0  # GX FIFO
```

## Applying it to another Wii game

Generic: the flat window, the alias mapping, the fault reporter and the
translator option (`translation.hardware_access_path` in `recomp.yml`) work
for any Wii title; the Vita fold works for any title that stays inside MEM1,
MEM2 and the locked cache.

Game-specific: the list of functions that touch hardware. Build it the way
this port did: run the game with a checked memory path that records every
access outside RAM by caller, add a static scan for functions that load a
hardware base (`lis` of `0xCC00`, `0xCD00`, `0xCD80`, `0xC800`, `0xE000`),
and let the fault reporter catch what both missed. A game that executes
code from RAM it writes (self-modifying code, code in MEM2 that is copied)
needs that handled separately; this runtime replaced write-protected code
pages with host-side checks and a code checksum in development builds.
