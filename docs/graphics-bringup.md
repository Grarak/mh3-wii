# Graphics bring-up evidence

MH3's renderer setup at `0x8003FD30` calls GX initialization at `0x804B3D10`
with a FIFO pointer from `r13-0x46C0` and a size of `0x80000`. The returned
FIFO object is stored at `r13-0x46BC`.

GX initialization establishes these guest MMIO pointers:

| Guest global | Hardware block |
| --- | --- |
| r13-0x3C54 | 0xCC000000 command processor |
| r13-0x3C58 | 0xCC003000 processor interface |
| r13-0x3C50 | 0xCC001000 pixel engine |
| r13-0x3C4C | 0xCC004000 memory interface |

The static FIFO object is at `0x807490E0`. Initialization calls `0x804B4C40`
with this object, base, and size, then `0x804B4DD0` and `0x804B4FB0` with the
object. These correspond to FIFO base, CPU FIFO, and GP FIFO setup. The host register interface now lets this setup execute. Boot completes GX initialization, the three initial display copies, and
blocking GXDrawDone. It now waits for VI retrace delivery.

Preserve MH3's own guest GX state initialization or reconstruct its verified
layout when replacing these boundaries. WiiCompiled's Mario Kart addresses and
object addresses cannot be reused.

Aurora's FIFO is not a transparent consumer of the original guest stream:
`lib/gx/command_processor.cpp` rejects ordinary `CP_REG_ARRAYBASE_ID` and
requires `GX_LOAD_AURORA_ARRAYBASE`, which carries a host pointer, size, and
endianness flag. The current bridge performs this conversion with a RAM-region
bound; a guest address is never cast directly to a host pointer. Textures, TLUTs, and display lists likewise require pointer and
object translation. WiiCompiled's `runtime/src/hle/gx` implements such bridges,
but with title-specific registrations and guest-state addresses.

The separate `--graphics-check` exercises host Metal initialization and frame
submission. It does not exercise these guest boundaries or prove game rendering.

The register masks were checked against
[Dolphin CommandProcessor.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/CommandProcessor.cpp).
The bridge intentionally leaves unsupported CP status/metrics and other MMIO
unmapped. The FIFO command decoder rejects TLUT/palette commands, display lists,
indexed XF loads, draw packets, texture/scaled copy triggers, and raw token/finish events until
those paths have validated conversions and completion handling.

The latest native boot consumed 298 commands through Aurora, including
16 converted vertex-array bindings and eight texture bindings, before the
display-copy guard stopped it.
This is setup-state verification, not a rendered game frame.

Texture image addresses (BP image3) are converted into Aurora's 0x50/0x0030
metadata packet. Image dimensions, format, and mip filtering come from shadowed
BP registers with one-shot write-mask semantics. The resolver validates the
whole tiled image/mip chain within guest RAM. A new texture revision on binding
prevents reusing cached data across reloads of the same guest address. Paletted
formats remain rejected until their TLUT storage and ownership are implemented.
The next boundary is GXCopyDisp at `0x804B7B80`, called three times by renderer
setup at `0x8003FE4C`, `0x8003FE58`, and `0x8003FE68`, followed by draw completion
at `0x804B67E0`.

The unscaled display-copy callback validates destination stride, height, and RAM
extent, then calls GXSetDispCopyDst/GXCopyDisp on Aurora. Aurora retains a GPU
presentation texture; guest XFB RAM is not filled with YUV output. The blocking
GXDrawDone replacement is registered at `0x804B67E0`. It flushes guest dirty
state at `0x804B6C70`, submits/finishes the host frame, waits for actual Dawn queue
completion, sets the SDK completion byte at r13-0x3C08, invokes the callback at
r13-0x3C04 with external interrupts masked, and wakes the queue at r13-0x3C10.
Caller CPU state is restored even if a callback throws.

A native sample after successful Metal completion located the next wait:
`8003F278 -> 8003FE94 -> 804E8050 (VIWaitForRetrace) -> 804D50B0 -> 804D4360`.
Raw asynchronous finish/token packets remain guarded until their event delivery
is implemented. Copy tests cover stride/address decoding, exact framebuffer
bounds, and rejection of unsupported scaling and texture-copy modes.

Retrace delivery is now cooperative at `VIWaitForRetrace` (`0x804E8050`). It
paces a host deadline, services IOS completions and window events, latches the
field boundary and VI interrupt status, then invokes the original handler at
`0x804E6F30` using the current guest OS context from `0x800000D4`. The caller CPU
state is restored, and the SDK counter must advance exactly once. Field/control
bits were checked against
[Dolphin VideoInterface.h](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/VideoInterface.h).
This does not provide arbitrary preemptive interrupts outside service boundaries.

The first handler invocation also required SISetXY (`0x804DEC10`) and queued STM
immediate controls. SISetXY retains the exact SDK polling word at `0x8062A8A4`.
After retrace, an absent boot FST caused DVDOpen to fail. Boot now installs the
real FST at the top of MEM1 and reduces both arena-high fields accordingly.
A sample then showed the module loader polling DVDGetCommandBlockStatus
(`0x804AACC0`); this boundary now pumps queued IOS callbacks before the original
status query, with nested pumping suppressed.

The next stop was `hbm_data.rso` prolog at `0x8081A8A4`. The original loader produced
module base `0x808196E0`, section 1 offset `0xF0`, and prolog offset `0x10D4`,
which exactly match the target. The file has 18 sections and its text section is
`0x2AC28` bytes. The first module is now statically translated, including its
constructor table, and executes successfully. See [RSO bring-up](rso-bringup.md).
The subsequent stop was locked-cache address `0xE0000000`, reached through
`80046CE4 -> 800CF448 -> 800D3330 -> 8007A464 -> 80502CF8 -> 804CCEF0`.

NW4R G3D initialization enables the locked L1 cache through `0x804CCEF0`.
Its helper at `0x804CCE20` clears 512 cache lines of 32 bytes beginning at
`0xE0000000`. The runtime now provides that separate 16 KiB region, with checked
bounds and no ordinary MEM1/MEM2 aliases. This handles the direct loads/stores
used during initialization; cache DMA and arbitrary BAT remapping are not yet
implemented. The independent locked-L1 address space is also described in
[Dolphin's memory map](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/Memmap.h).

The subsequent indirect jump to `0x804C2AE0` is an allocator vtable thunk, not a
switch case. The table at `0x8079F730` is installed by `0x804C2B20` and contains
allocation/free tail thunks at `0x804C2AE0` and `0x804C2AF0`. Both are now explicit
function seeds, since neither has a stack prologue.

PAD startup then queries the four GameCube SI channels. The native SIGetType
binding at `0x804DF360` reports `SI_ERROR_NO_RESPONSE` (8) and updates the SDK
cached type at `0x8062A8B8 + 4*channel`. The original SIGetTypeAsync sees a completed
query and invokes its original callback with interrupts masked. This represents
absent physical GameCube devices; it does not provide host PAD/WPAD input.

The leaf wireless initialization callback at `0x80479C20` is also now an explicit
seed, verified in the five-entry callback table at DOL file offset `0x56FDF8`.
The next verified boot stop was opening Bluetooth USB device `/dev/usb/oh1/57e/305`
from `0x804E5250`.


WUD startup is now a native library boundary at `0x804FD450`, following
WiiCompiled's use of host controller services. It preserves the SDK's startup
flag and ready status at `0x80761668`, with zero connected remotes. The original
WPAD initializer still initializes its four channel blocks, callbacks, settings,
and polling alarm. Raw Bluetooth USB transport is not implemented. Host
keyboard/gamepad sample delivery and periodic WPAD servicing remain unfinished.

The initializer's sensor-bar GPIO output (bit 8 of `GPIOB_OUT`, physical
`0x0D0000C0`) is latched across its valid register aliases. Other output devices
are explicitly rejected. Register identity is documented in
[Dolphin WII_IPC.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/WII_IPC.cpp).
Tests cover WUD startup/idempotence and GPIO aliasing/rejection.

The next boot reached the AI library initialization banner and stopped at
`0xCD006C00` from `0x8046DC00`, called through
`80046CE4 -> 800EEB44 -> 800E8794`. AI register support now passes that point;
see [audio bring-up](audio-bringup.md) for the current DSP boundary.


Display-copy Y scaling now converts the EFB source interval count to XFB lines
before checking destination RAM and passing dimensions to Aurora. BP copy bit 10
selects reciprocal versus direct scaling; reciprocal zero is rejected. This
matches the copy path in [Dolphin BPStructs.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/BPStructs.cpp).
Unlike the SDK allocation helper, this command bridge does not round the copy
height up to an allocation estimate. Tests cover direct and reciprocal scaling,
fractional truncation, zero scale, and exact/one-byte-short destination buffers.
Both native and Metal builds pass their runtime and audio tests. The Metal boot
passes the previously rejected scaled copy and completes a second GPU wait; it
then stalls before another retrace or audio completion callback.
The process sample locates the stall in the original scheduler:
`800401A8 -> 804D5320 -> 804D4F20 -> 804D4360` (the last function spins).
This is the next thread/event integration boundary, not a Metal queue wait.


The scheduler stall above originates in `OSSleepTicks` (`804D5320`). A native
host-timed wait now services IOS completions, audio, alarms and retraces at
roughly 1 ms intervals. It preserves CPU state and explicitly rejects runnable
threads requiring a fiber switch. This is not yet a general thread scheduler.

In PAL startup MH3 uses its periodic OS alarm (`8003F590`) for the 60 Hz frame
counter, rather than VI alone. Host decrementer delivery now consumes the
original SDK alarm queue at r13-3B10, preserving the original setters,
cancellation and periodic InsertAlarm routine (`804CBB30`). Callback delivery
uses a temporary guest context, masks interrupts, disables scheduling during
the callback, and restores state even when a callback fails. A reentrancy guard
and bounded queue draining prevent recursive servicing. Tests cover one-shot
removal, periodic rearming, early delivery prevention, original cancellation,
CPU/context restoration and callback exceptions.

The Metal boot now delivers both MH3's frame-timing alarm and the original
WPAD polling callback (`804ED6B0`). It advances to GX opcode `0x80` (primitive
packet), LR `804B6F2C`, after 439 commands and four display copies. Vertex packet
decoding is the next graphics boundary. No gameplay is claimed.


## Guest drawing and framebuffer texture copies

Vertex packets (`80..BF`, including all eight VAT slots) now accumulate until
their complete payload is present. The framer tracks CP vertex descriptors and
VAT A/B/C words, with matrix-index bytes, direct numeric/color formats, index8,
index16, and NBT3's three indices. It preserves bytes for Aurora's existing
vertex decoder and allows the largest 16-bit-count packets within a bounded
16 MiB buffer. Tests cover fragmented headers/registers/payloads, all primitive
opcodes, cross-word texture formats, an oversized packet, and the following
command boundary.

Color EFB-to-texture copies now translate source/destination dimensions and
format bits, validate tiled RAM footprints and row strides, and use Aurora's
`GXCopyTex`. Immediate `aurora_flush_efb_copy_to_ram` downloads Wii-tiled data
before guest execution resumes, keeping even inlined RAM reads coherent. This
is deliberately synchronous and may be expensive. I4/I8/IA4/IA8/RGB565/RGB5A3/
RGBA8 contiguous output is supported, including half-scale copies; depth,
channel-select and strided output remain explicit boundaries. Tests cover all
seven supported formats, exact/short RAM bounds, stride rejection and half scale.

The real Metal boot renders the Nintendo wrist-strap screen. Capture evidence:
`logs/mh3-frame-100.png`, converted from Aurora's BMP capture. Both native and
Metal test suites pass. The next stop is `Unimplemented direct IPC register
operation`, LR `804BBF60`, request buffer `935E0020`. Before that stop the bridge
consumes 122703 commands, 1286 draw packets, 645 texture copies and 649 display
copies. Input and gameplay are still unverified.

To capture a frame, set `MH3_CAPTURE_PATH` to an absolute BMP output path and
optionally `MH3_CAPTURE_FRAME` (default 30) when launching the Metal build.


The following IOS bridge now passes the raw IPC boundary above: `IOSIoctlvAsync`
at `804BD220` queues results from the existing synchronous handler. FS command
12 implements directory usage (16 KiB clusters and file/directory counts),
using descriptor-relative traversal with no symlink following. Missing paths
return errors; no save data or usage values are fabricated. Tests cover nested
accounting, asynchronous success/error results, invalid paths and symlink
isolation, including rejecting repeated leading slashes in file opens.
Protocol references: [Dolphin FileSystemProxy.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/IOS/FS/FileSystemProxy.cpp)
and [HostBackend/FS.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/IOS/FS/HostBackend/FS.cpp).

The next actual boot stop is asynchronous `/dev/fs` command 3 (create directory),
LR `804B26AC`, input `935E1020`, length `4C`, callback `804B2450`. The boot reaches
652 display copies, 1292 draw packets and 648 texture copies. Save-filesystem
creation and metadata, plus host input and further game initialization, remain
unfinished.

## Opening movie and locked-cache DMA

SPR 922/923 now implement the transfers issued by MH3's original LCLoad/LCStore
code. DMAU supplies the physical RAM address and upper length bits; DMAL supplies
the cache address, lower length bits, direction, and trigger. Zero encoded length
means 128 32-byte lines. Copies complete synchronously and clear the trigger;
the queue count is zero. Hardware transfer latency is not modeled.

The bridge validates entire RAM/cache ranges before copying and retains the
executable-write guard. It supports MEM1/MEM2 and the allocated 16 KiB locked
cache; MMIO/EFB DMA and other cache ranges still fail explicitly. Register fields
were checked against [Dolphin's Broadway declarations](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/PowerPC/Gekko.h)
and the local SDK instructions at `0x804CCF60` and `0x804CCF90`.

Tests cover both directions, split length fields, zero/max length, byte order,
trigger/completion state, and invalid ranges. Native and Metal runtime/audio tests
pass. Real startup logs 4096/4096/2048-byte cache-to-MEM2 transfers and renders the
opening movie's underwater scene at captured frame 1100. Local evidence:
`logs/lc-dma-boot.log` and `logs/lc-dma-frame-1100.png`. A rendered frame verifies
video output, not complete movie playback, audio synchronization, or gameplay.

For a sequence of actual rendered frames, set `MH3_CAPTURE_PATH` to a BMP path
and `MH3_CAPTURE_INTERVAL` to a positive frame interval. For example,
`MH3_CAPTURE_PATH=logs/startup.bmp MH3_CAPTURE_INTERVAL=500 build/metal/MH3`
writes `startup-500.bmp`, `startup-1000.bmp`, and so on. Without the interval,
the existing `MH3_CAPTURE_FRAME` option still captures one frame. Captures are
requested before Aurora seals the frame, accounting for its frame-index advance.
The first two captures were checked during a real native boot.

## Display lists and indexed XF loads

Character-scene setup reaches `GX_CMD_CALL_DL` (`0x40`). The guest stream now
resolves its big-endian address/byte count against RAM and recursively processes
the list through the same bridge. Nested lists preserve CP/BP state, masks and
texture revisions, then return to the parent FIFO stream. Raw guest display-list
pointers never reach Aurora's decoder. Lists must end on a command boundary;
RAM bounds, the existing 16 MiB packet limit, and a 16-level nesting limit are
checked. Tests cover fragmented calls, nested ordering/state, short RAM views,
truncated commands and self-recursion. The live display-list boot progressed to
indexed XF opcode `0x20` (`logs/display-list-ready.log`).

Indexed XF opcodes `20/28/30/38` now resolve the appropriate CP matrix-array base
and stride, check the complete source extent, and emit an equivalent direct XF
load with the original big-endian bytes. Supported destinations match Aurora's
full position, normal, texture and post-transform matrices plus in-light ranges.
Unsupported partial matrices/destinations fail explicitly instead of allowing
Aurora's decoder to skip them. Tests cover all four selectors, fragmented
commands, source offsets, byte order, short bounds and partial-matrix rejection.

Both build variants pass their runtime/audio tests (plus SDL input tests on
Metal). The indexed-XF boot executes these commands and reaches Aurora draw
admission (`logs/indexed-xf-ready.log`), which rejects an oversized storage
allocation. The current vertex-array conversion supplies the entire remaining
RAM region as `array.size`; Aurora's `admit_draw` and `handle_draw_unmerged`
reserve/upload that whole region into an 8 MiB staging buffer. The next change
must derive the needed array extents from actual draw indices and element
formats rather than increasing buffers or truncating arrays arbitrarily.

## Draw-scoped vertex-array bounds

The framer now shares one VAT/VCD attribute layout between packet sizing and
index scanning. Before each indexed draw, it supplies Aurora with only the
prefix ending at the highest referenced element, including the element's full
byte extent. NBT3 uses three independent three-component elements; ordinary NBT
uses one nine-component element. Direct attributes and matrix-index bytes are
skipped correctly. Each extent is checked against accessible guest RAM. Buffer
capacities are unchanged.

Tests cover mixed matrix/direct/index8/index16 framing, all attributes, NBT and
NBT3 extents, smaller subsequent draws, and rejection before an out-of-RAM draw
is submitted. Native and Metal test suites pass. The live run
(`logs/array-range-ready.log`) now renders the animated character, passing the
previous storage-capacity failure. Captures show repeated smaller images and
missing UI, so frame/copy sequencing still needs correction before gameplay is
usable. Evidence includes `logs/array-range-ready-4200.bmp` and later frames.

## GPU fences and display presentation

GXDrawDone now submits intermediate work synchronously and resumes the same
EFB without presenting or advancing the frame. Only a pending GXCopyDisp ends
the host frame. Subsequent host frames preserve EFB contents; guest copy-clear
commands determine when to clear them. A local Aurora source overlay shares
Aurora's submission lock and leaves the sibling checkout unchanged.

The actual Metal EFB test checks preservation across an intermediate fence and
frame-index advancement at display completion. All three Metal CTests pass.
The live run (`logs/frame-fence-boot.log`, frame 1717) reaches character creation
with its complete UI and correctly colored skin; the recursive smaller images
are gone. This verifies character creation rendering, not gameplay.

## Staging-capacity submissions

Aurora's decoder returns before a draw when its current staging batch is full.
The guest bridge now calls Aurora's `split_staging_batch` outside the decoder
lock and retries that complete command once. This preserves the active EFB,
viewport/scissor, and presentation state without advancing the display frame.
A second failure remains an explicit error rather than an unbounded retry.

All three Metal CTests pass. The actual Metal EFB check also forces a split and
verifies retained pixels and the unchanged frame counter. The live run in
`logs/staging-split-boot.log` passes the former packet-consumption failure and
reaches 2,609 display copies before encountering a guest TLUT address requiring
host-pointer conversion. Village gameplay remains unverified.

## Indexed textures and palette memory

The guest framer supports C4, C8, and C14X2 texture footprints and snapshots
TLUT DMA transfers into persistent palette memory. It interprets the guest
transfer count in 32-byte lines and the destination in 512-byte units, then
selects palette format from each texture unit's TLUT register. These hardware
fields are cross-checked against Dolphin's
[BP definitions](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/BPMemory.h)
and [TLUT loading](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/BPStructs.cpp).

Before sampled indexed draws, the bridge supplies Aurora with a host palette
pointer, format, entry count, and revision. Palette-only updates also refresh
the texture revision to invalidate Aurora's resolved-texture memo. Overlapping
uploads preserve untouched palette bytes, and display-list calls share palette
state. Unsupported formats, short source RAM, and incomplete palettes fail
explicitly. Palettes currently require all entries for their texture format
to have been loaded.

Both runtime suites pass. Regressions cover indexed texture sizes, fragmented
loads, RAM snapshots, palette cache reuse/reload, IA8/RGB565/RGB5A3 metadata,
overlapping updates, display-list state, unloaded palettes, and short RAM.
The final live run (`logs/tlut-final-boot.log`) passes the palette boundary and
executes 1,844,379 draws before a separate unrecognized 635-entry switch jumps
to `0x80231000`. Its last capture is black; village gameplay is not verified.
