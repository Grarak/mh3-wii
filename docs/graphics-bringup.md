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
object. These correspond to FIFO base, CPU FIFO, and GP FIFO setup. The current
boot stops in the CPU FIFO path on command-processor register `0xCC000002`.

Preserve MH3's own guest GX state initialization or reconstruct its verified
layout when replacing these boundaries. WiiCompiled's Mario Kart addresses and
object addresses cannot be reused.

Aurora's FIFO is not a transparent consumer of the original guest stream:
`lib/gx/command_processor.cpp` rejects ordinary `CP_REG_ARRAYBASE_ID` and
requires `GX_LOAD_AURORA_ARRAYBASE`, which carries a host pointer, size, and
endianness flag. Textures, TLUTs, and display lists likewise require pointer and
object translation. WiiCompiled's `runtime/src/hle/gx` implements such bridges,
but with title-specific registrations and guest-state addresses.

The separate `--graphics-check` exercises host Metal initialization and frame
submission. It does not exercise these guest boundaries or prove game rendering.
