# Long-lived switch-table bases

> Platform note: results in this document were recorded on the earlier macOS
> ARM64/Metal build. The port now targets arm64 Linux with Vulkan; see
> [arm64 Linux and Vulkan](linux-arm64.md) for what has been re-verified.

Village loading reaches the main-DOL rendering function at `0x800AC35C`.
Its switch at `0x800ACB78` loads a target from `0x80593A28` using selector
`r26`. The table's page register `r22` is initialized at `0x800AC6F4`, beyond
the translator's 192-instruction local search. The unresolved jump stopped at
case `0x800ACBF8` (`logs/map-village-boot.log`).

The MH3-local translator adds a fallback for an `addi` based on a nonvolatile
GPR with an earlier `lis` definition. It follows the decoded control-flow
paths from the function entry, requiring the definition on every path to the
use. Calls preserve the nonvolatile register under the PPC ABI; intervening
writes, update-address loads/stores, and multi-register loads invalidate it.
The existing bounded recognition, selector bounds, table reads, and target
validation remain in place.

The case is now emitted as a local C++ switch/goto inside the original
function, preserving its resident registers and stack. It is not registered
as a standalone function or replaced by a native stub. The switch-detector
suite has 17 passing cases, including long-distance definitions, read-only
stores, calls, register clobbers, update stores, load-multiple, and paths that
bypass initialization. Translation continues to report zero unsupported
instructions and zero invalid SSA functions.

Both rebuilt runtime test suites pass. The Metal run
`logs/long-switch-boot.log` passes the former indirect jump and advances to
2,529 display copies before a separate GX packet-consumption mismatch.
Gameplay remains unverified.

## Large item switch

After palette uploads were implemented, Village startup reached the item
dispatch in `0x80229F30`. Its `bctr` at `0x80229FDC` indexes the table at
`0x805C1D68`, bounded by `cmplwi r5,0x27A`: 635 entries and 251 distinct
targets. The former 512-entry scan limit left it unresolved. Selector one
points to the containing function's epilogue at `0x80231000`, which must
execute with that function's resident registers and stack.

The bounded table limit is now 4,096 entries (16 KiB). Every entry is still
read and validated before recognition succeeds. Tests cover the real table
size, the new maximum, exceeding the maximum, and an invalid final entry;
all 21 switch-detector tests pass. Regeneration emits the missing target as
a local switch/goto and retains zero unsupported instructions and invalid
SSA functions.

Both rebuilt runtime suites pass. The live run `logs/large-switch-boot.log`
passes the former dispatch failure and renders the player, village scenery,
NPCs, and Village Chief dialogue (frame 2121). Further interaction validation
is tracked in the project README.
