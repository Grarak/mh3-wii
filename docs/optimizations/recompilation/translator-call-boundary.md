# Translator call boundary: registers instead of the context

## What the original does

A PowerPC function passes arguments in r3-r10 and f1-f8, returns in r3/r4
or f1, and preserves r14-r31, f14-f31 and parts of CR across calls (the
EABI). WiiCompiled's translator models the guest register file as a
`CpuContext` in memory: a translated function loads the registers it uses
into C++ locals and stores them back around every call, and a call whose
effects it cannot prove (an indirect `bctrl`, a native) gets a
complete-context fence. Indirect calls look the target up in a generated
table, and natives were entered through a registry lookup and a guard that
saved and restored r14-r31 and f14-f31 (removed since, see below).

## What the port does instead

The translator in `toolchain/translator` (changed in this repository) and
the runtime's dispatch were changed so that values stay in host registers
across calls wherever the guest ABI already guarantees it:

| Change | What it does | Source |
| --- | --- | --- |
| Non-volatile residency | `NonvolatileGprPreservationAnalyzer` proves which of r14-r31 a function restores on every exit; callers keep those in locals across the call. 18,857 of 23,151 base functions preserve all of them. | `Analysis/NonvolatileGprPreservationAnalyzer.cs` |
| Register arguments | A function with a register contract gets a variant `func_X_ra...` whose first GPR and FPR arguments and results travel in AAPCS registers; statically bound direct calls use it. | `CodeGen/CxxLinearCodeGenerator.RegArgs.cs`, `runtime/include/abi_bridge.h` |
| EABI boundary for indirect calls | An unknown `bctrl`/`blrl`/`bctr` target is assumed to follow the EABI, so call sites sync r0-r13, f0-f13 and special registers instead of the whole context. Fenced functions fell from 11,049 to 1,378. | `Analysis/GuestAbiContractAnalyzer.cs` |
| Precise natives | Natives whose contract lists exactly the registers they touch are called directly, without the registry lookup (`precise_native_calls.inc`). | `Translator.Cli/Program.cs` |
| No register guard | A native leaves r14-r31 and f14-f31 as the compiled function it replaces did, so no call into one saves and restores them. A native that switches context declares `MH3_NATIVE_FULL_CONTEXT`. | `runtime/include/abi_bridge.h` |
| Indirect-call memo | 2048 entries as 1024 two-way sets; a hit on a translated target needing no guard runs inline. | `runtime/include/abi_bridge.h` (`InvokeIndirectCpu`) |

The changes rely on invariants that the commits state and fix where they
were wrong: `CpuContext` rN (r14-r31) is written only by functions that do
not preserve rN, and a preserving function stores its entry value back at
every exit (commit "Restore preserved nonvolatile GPRs a body publishes
before it exits": a pass-through register read back a callee's value and
faulted after minutes of play).

## Why

The translated code's overhead was in its call boundaries. Measurements
recorded in the commit messages (Alatreon game thread, ms per frame, the
host of that time):

| Commit | Before | After |
| --- | --- | --- |
| "Call precise natives directly from translated code" | 8.27 | 7.83 |
| "Keep r14-r31 in host locals across calls that preserve them" | 6.81 (translated code 3.71) | 6.65 (3.48) |
| "Pass translated call arguments and results in host registers" | 6.75 | 6.42 (within noise, +12% code size) |
| "Make the indirect dispatch memo two-way" | about 60 memo misses a frame | below 3; miss path 0.023 → about 0 ms/frame (Cortex-A9) |
| "Drop the register guard around calls into natives" (Vita, Deserted Island beach) | guarded dispatch 3.7% of the game thread, frame 13.98 ms | 0.5%, 13.55 ms |

Before the guard was dropped, a gdb script compared r14-r31 and f14-f31
around every guarded call: none changed in 1.58 M calls at the beach,
0.99 M in the village, 0.93 M in the Alatreon fight and 1.60 M in the
arena.

## Example

The contract test that decides whether a native skips the guard is a
statement about its source, not a runtime check. A precise native reads its
arguments from constant register indices and passes values, not the
context:

```cpp
// precise: the translator lists r3 and r4 as its inputs and r3 as its output
cpu->gpr[3] = Accessor(cpu->gpr[3], cpu->gpr[4]);

// not precise: Port can touch any register, so the call goes through the
// generic dispatch
Port(cpu);
```

Making the hottest imprecise natives precise (their bodies name only
argument registers) took the Jhen Mohran fight's game logic on the Vita
from 13.28 to 12.50 ms a frame.

## Applying it to another Wii game

Generic: all of these are translator and runtime changes that depend only on
the PowerPC EABI that Metrowerks CodeWarrior and the Nintendo SDK follow.
They apply to any game built with that toolchain.

Game-specific risks: an indirect callee that does not follow the EABI
(hand-written assembly), which the boundary trusts; a preservation pattern
the analyzer does not know (it recognizes stack save slots, the
`_save_gpr_N`/`_rest_gpr_N` thunks and callee contracts); and jump tables
whose targets the translator cannot bound (an unregistered jump target stops
the game, so every `bctr` that runs is a known function). Check
a new game with the translator's own reports (unsupported instructions,
invalid SSA, fenced function counts) before relying on the faster boundary.
