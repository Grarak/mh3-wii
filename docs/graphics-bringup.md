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
object. These correspond to FIFO base, CPU FIFO, and GP FIFO setup. The host register interface now lets this setup execute. Boot completes GX initialization and stops on the first display-copy trigger
(`BP 0x52`) in the game renderer setup.

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
indexed XF loads, draw packets, copy triggers, and token/finish events until
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
