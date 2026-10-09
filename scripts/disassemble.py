#!/usr/bin/env python3
"""Usage: tools/python/bin/python scripts/disassemble.py ADDRESS [BYTE_COUNT]."""
import struct
import sys
from pathlib import Path
from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN

root = Path(__file__).resolve().parents[1]
data = (root / "Assets/DATA/sys/main.dol").read_bytes()
address = int(sys.argv[1], 0)
size = int(sys.argv[2], 0) if len(sys.argv) > 2 else 256
for index in range(18):
    offset = struct.unpack_from(">I", data, index * 4)[0]
    base = struct.unpack_from(">I", data, 0x48 + index * 4)[0]
    length = struct.unpack_from(">I", data, 0x90 + index * 4)[0]
    if base <= address < base + length:
        end = min(address + size, base + length)
        decoder = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
        decoder.skipdata = True
        for ins in decoder.disasm(data[offset + address - base:offset + end - base], address):
            print(f"{ins.address:08X} {ins.mnemonic:12} {ins.op_str}")
        break
else:
    raise SystemExit(f"Address {address:#x} is outside DOL sections")
