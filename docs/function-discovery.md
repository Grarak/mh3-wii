# Additional indirect function discovery

> Platform note: results in this document were recorded on the earlier macOS
> ARM64/Metal build. The port now targets arm64 Linux with Vulkan; see
> [arm64 Linux and Vulkan](linux-arm64.md) for what has been re-verified.

The RMHP08 SEL exports and standard stack-prologue scan omit leaf virtual methods.
Entering save selection exposed two such calls:

- `0x80506DFC` dispatched to font getter `0x80507094` through the vtable entry at
  `0x8063053C`. The method loads the font resource's halfword at offset `0x10`.
- After translating that table, `0x80448EE8` dispatched to UI setter `0x8055E394`
  through the entry at `0x80653E20`. The method stores its argument at object
  offset `0x34`.

`scripts/generate_symbols.py` now reads the font vtable and the two related UI
vtables from the pinned DOL. Explicit ranges exclude their zero headers and
trailers, and each pointer must be aligned and inside an executable section.
These are static translation seeds; no interpreter, runtime code generation, or
replacement of the original leaf methods is involved.

The font-only build passed both runtime/audio test suites and progressed to the
UI setter (`logs/font-ready.log`). Subsequent validation is recorded below.

The next boot reached `0x8053C258`, another leaf method, from an indirect call at
`0x8055BF6C` (`logs/saveui-ready.log`). To avoid discovering these one boot at a
time, the symbol generator additionally treats aligned DOL data pointers to text
immediately after `blr` or `bctr` as speculative seeds. It does not assign SDK
names to them. Pointers lacking that return boundary, unaligned pointers, and pointers
outside text are excluded by this rule; a synthetic regression covers those
cases. A return boundary remains a heuristic, not proof of a function entry. The strict translator remains responsible for decoding/validating the
candidate and repairing affected function boundaries. This supplements the
existing speculative stack-prologue scan rather than proving whole-game
function coverage.

The return-boundary build passes native and Metal tests and proceeds to
`0x8054FEE0`, called at `0x8055B59C` (`logs/leaf-ready.log`). This is a C++
multiple-inheritance thunk: `addi r3,r3,-0x10` followed by a tail branch. Discovery
also recognizes data pointers to this exact two-instruction pattern, checking
that the branch is relative, has no link, and resolves inside executable text.
Tests reject linked calls, other argument-register adjustments, and branches
outside text.

The thunk build progresses to virtual forwarder `0x805575A4`, called at
`0x8055D928` (`logs/thunk-ready.log`). Its vtable begins with an already recognized
adjustment thunk. The generator therefore also seeds consecutive executable
pointers following a two-word zero header when the first method is already a
recognized seed. At least two pointers are required. This is explicitly a
heuristic; tests cover the header, first-method requirement, single-entry
rejection, and invalid-pointer termination.

Final validation: 23,142 translated functions, zero unsupported instructions and
zero invalid SSA functions. The Metal runtime/audio/SDL tests pass. A live Metal
boot renders all six save slots (`logs/vtable-ready-1500.bmp`), accepts File 1,
renders its file menu (`logs/vtable-ready-2100.bmp`), and reaches the main menu
(`logs/vtable-ready-2500.bmp`). Selecting New Game proceeds to direct framebuffer
access and stops at guest address `0xC8000000` in translated function `0x80056F88`;
this is the next graphics boundary, not a missing translated function.
