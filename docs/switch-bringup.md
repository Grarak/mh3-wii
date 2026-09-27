# Long-lived switch-table bases

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
