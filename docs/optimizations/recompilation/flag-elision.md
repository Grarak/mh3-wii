# Condition flags only where something reads them

## What the original does

PowerPC compares and carrying arithmetic set condition register fields
(CR0-CR7) and the carry (XER.CA); a branch or `mfcr`, `adde` and the like
read them later. On the hardware a flag update costs nothing extra.

## What the port does instead

The translator computes a guest flag only when a later instruction can read
it. Its flag elision (`CxxLinearCodeGenerator.FlagElision.cs` in
`toolchain/translator`) used to keep every CR field and the carry live at a
return, and to treat most calls as reading all of them: 13,469 callee
contracts claimed to read all eight CR fields because an indirect call
somewhere below them read everything. Nearly every compare and carry update
that reached a call or a return was still computed.

Flag liveness now follows the PowerPC EABI: a callee clobbers CR0, CR1,
CR5-CR7 and XER.CA and preserves CR2-CR4, and of the caller's flags it reads
only CR1 (bit 6 marks a varargs call). So:

- a return publishes CR2-CR4 only;
- a guest call (not the register save/restore thunks) kills the volatile
  flags and reads at most CR1-CR4; an indirect call reads CR1-CR4;
- an indirect jump still reads everything, since an unresolved jump table
  may land in the function's own code;
- a phi renames a field instead of reading all of them, and the CR logic
  ops (`cror`, `crclr`, `mcrf`, `mcrxr`, `mcrfs`, the paired-single
  compares) and `mtcrf` read only the fields they touch.

`MH3_CR_EXIT_AUDIT=1` (translator) lists every call after which a volatile
flag is still read, with the reader: across all profiles only `mfcr` copies
of the whole CR after the caller rewrote CR0, which follow the EABI too.

## Why

| | Before | After |
| --- | ---: | ---: |
| `SetCR` sites in the base translation | 85,946 | 6,481 |
| Carry updates | 824 | 126 |
| Vita, Jhen Mohran fight: game logic per frame | 12.48 ms | 11.48 ms |
| Vita, Jhen Mohran fight: logical fps | 57.9 | 61.9 |

The Vita figures include the stfs narrowing fold of the next commit
([Cortex-A9 floating point](cortex-a9-floating-point.md)). On a Cortex-A9
each dead flag update is a compare and a few moves and stores into the
context, on every pass through the code.

## Example

What a function's exit keeps:

```text
before: return reads CR0-CR7, XER.CA   -> every compare before it stays
after:  return reads CR2-CR4           -> cmpwi cr0 / addc results die at the return
```

## Applying it to another Wii game

Generic: CodeWarrior and the Nintendo SDK follow the EABI's CR and XER
rules, so the same liveness applies to any game built with them. Run the
exit audit on a new game: hand-written assembly that reads a volatile field
across a call would show up there.

Game-specific: nothing beyond the audit's findings.
