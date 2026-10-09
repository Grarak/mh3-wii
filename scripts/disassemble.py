#!/usr/bin/env python3
"""Disassemble a guest DOL range with Capstone from tools/python."""
import argparse
import struct
from pathlib import Path
from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("address", type=lambda value: int(value, 0))
parser.add_argument("byte_count", nargs="?", type=lambda value: int(value, 0), default=256)
parser.add_argument("--dol", type=Path, default=root / "Assets/DATA/sys/main.dol")
args = parser.parse_args()
data = args.dol.read_bytes()
address = args.address
size = args.byte_count
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
