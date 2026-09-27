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
