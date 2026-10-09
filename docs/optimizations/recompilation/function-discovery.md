# Finding every function and jump target ahead of time

## What the original does

The game reaches much of its code indirectly: C++ virtual calls through
vtables, multiple-inheritance thunks and `bctr` jump tables. On the Wii the
CPU simply fetches whatever address it is given. A static recompiler only
has the code it translated, so every indirect target must be known at build
time.

## What the port does instead

`scripts/generate_symbols.py` seeds the translator with function entries
beyond the SEL exports and the stack-prologue scan, all read from the
pinned DOL:

- **Vtables.** Explicit vtable ranges (the font and UI vtables that boot
  needs), each pointer aligned and inside an executable section.
- **Return boundaries.** Any aligned data pointer into text that lands right
  after a `blr` or `bctr` is a speculative seed. This finds leaf virtual
  methods that have no prologue.
- **Adjustment thunks.** Data pointers to `addi r3,r3,-N` followed by an
  unlinked relative branch inside text (C++ multiple-inheritance thunks).
- **Vtable runs.** Consecutive executable pointers after a two-word zero
  header, when the first method is already a recognized seed (at least two
  pointers).

These are heuristics; the strict translator decodes and validates each
candidate and repairs the function boundaries it affects.

Jump tables are recognized inside the translator (`toolchain/translator`):

- **Long-lived table bases.** A switch whose table register was set by a
  `lis` far before the `bctr` (beyond the 192-instruction local search) is
  resolved by following every control-flow path from the function entry;
  calls keep the nonvolatile register under the EABI, while writes,
  update-form loads and stores and load-multiple invalidate it. Example:
  `0x800ACB78`, table `0x80593A28`, base set at `0x800AC6F4`.
- **Large tables** up to 4,096 entries (16 KiB), every entry read and
  validated. Example: the item dispatch at `0x80229FDC`, 635 entries, 251
  distinct targets.

A recognized switch is emitted as a local C++ `switch`/`goto` inside the
original function, so a case that jumps into the function's epilogue runs
with its registers and stack.

## Why

Without an interpreter, a missed target stops the game ("Missing translated
target"). Finding targets one boot at a time does not scale; the seeds above
cover classes of targets at once. The result for MH3: 23,142 translated
functions, no unsupported instructions, no invalid SSA, and no missing
targets in play.

## Example

A thunk seed must be exactly this two-instruction pattern
(`scripts/generate_symbols.py`): `addi r3,r3,-0x10` then `b target`, with
the branch relative, unlinked and resolving inside executable text. Linked
calls, other register adjustments and branches outside text are rejected
(see `scripts/test_function_seeds.py`).

## Applying it to another Wii game

Generic: all of it. Start with the SEL exports, the prologue scan and the
seed classes above, translate, and run. Every "Missing translated target"
names the address and the caller; decide whether it is a new class of seed
(add a rule) or a one-off (add the address to `symbols.txt`).

Game-specific: which vtables exist and where. Pin the DOL by hash so that a
different disc revision cannot reuse the seeds silently.
