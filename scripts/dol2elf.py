#!/usr/bin/env python3
"""Convert the game DOL into a big-endian PowerPC ELF for Ghidra/objdump.

Each DOL text/data section becomes a PROGBITS section (plus .bss), and every
name from build/MAP.txt and symbols.txt becomes a FUNC symbol, so disassemblers
start with our labels. Data names exported by the disc's mh3.sel become OBJECT
symbols. The ELF is an analysis artefact, not something to run.
"""
import argparse
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--dol", type=Path, default=ROOT / "Assets/DATA/sys/main.dol")
parser.add_argument("--out", type=Path, default=ROOT / "build/ghidra/main.elf")
parser.add_argument("--inventory", type=Path, default=ROOT / "build/disc-inventory.json",
                    help="disc inventory holding the parsed mh3.sel static exports")
parser.add_argument("--symbols", type=Path, nargs="*", default=[ROOT / "build/MAP.txt", ROOT / "symbols.txt"])
args = parser.parse_args()

dol = args.dol.read_bytes()
offsets = struct.unpack_from(">18I", dol, 0)
addresses = struct.unpack_from(">18I", dol, 0x48)
sizes = struct.unpack_from(">18I", dol, 0x90)
bss_address, bss_size, entry = struct.unpack_from(">3I", dol, 0xD8)

sections = []  # (name, type, flags, address, bytes)
for i in range(18):
    if not sizes[i]:
        continue
    text = i < 7
    name = f".text{i}" if text else f".data{i - 7}"
    flags = 0x2 | (0x4 if text else 0x1)  # ALLOC | EXECINSTR or WRITE
    sections.append((name, 1, flags, addresses[i], dol[offsets[i]:offsets[i] + sizes[i]]))
sections.append((".bss", 8, 0x3, bss_address, b"\0" * 0))

symbols = {}
for path in args.symbols:
    if not path.exists():
        continue
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) >= 2 and not line.startswith("#"):
            try:
                symbols[int(parts[0], 16)] = parts[1]
            except ValueError:
                pass

# mh3.sel numbers sections as in Capcom's mh3.elf, not by DOL slot. Only the
# data sections that carry exports are listed; each base was checked against a
# global the code uses (system_w is the block the main loop clears at
# 0x8065AA60, Rmode is the render-mode pointer at r13-0x46B8, set_widemode_param
# is the byte at r13-0x7FFF the frame pacer reads). .text exports (sections 1-2)
# are already in build/MAP.txt.
SEL_DATA_BASES = {
    6: 0x8057CFE0,   # .data
    7: 0x8065A980,   # .bss
    8: 0x807932E0,   # .sdata
    11: 0x80796C20,  # .sbss
}
objects = {}
if args.inventory.exists():
    import json
    for export in json.loads(args.inventory.read_text())["static_exports"]["exports"]:
        base = SEL_DATA_BASES.get(export["section"])
        if base is not None:
            objects[base + export["offset"]] = export["name"]
        elif export["section"] == 0xFFF1:  # SHN_ABS: _SDA_BASE_, _SDA2_BASE_
            objects[export["offset"]] = export["name"]


def section_index(address):
    for index, (_, _, _, base, data) in enumerate(sections[:-1]):
        if base <= address < base + len(data):
            return index + 1
    return 0xFFF1  # SHN_ABS


shstrtab = bytearray(b"\0")
strtab = bytearray(b"\0")
symtab = bytearray(b"\0" * 16)
entries = {address: (name, None) for address, name in symbols.items()}
entries.update({address: (name, 1) for address, name in objects.items() if address not in symbols})
for address, (name, forced) in sorted(entries.items()):
    shndx = section_index(address)
    kind = forced or (2 if shndx != 0xFFF1 and sections[shndx - 1][2] & 0x4 else 1)  # FUNC / OBJECT
    symtab += struct.pack(">IIIBBH", len(strtab), address, 0, (1 << 4) | kind, 0, shndx)
    strtab += name.encode() + b"\0"

out = bytearray(52)
layout = []
for name, kind, flags, address, data in sections:
    offset = len(out)
    out += data
    layout.append((len(shstrtab), kind, flags, address, offset, bss_size if kind == 8 else len(data), 0, 0, 32, 0))
    shstrtab += name.encode() + b"\0"
for name, kind, data, link, entsize in [(".symtab", 2, symtab, len(sections) + 2, 16),
                                        (".strtab", 3, strtab, 0, 0), (".shstrtab", 3, shstrtab, 0, 0)]:
    name_offset = len(shstrtab)
    shstrtab += name.encode() + b"\0"
    offset = len(out)
    payload = shstrtab if name == ".shstrtab" else data
    out += payload
    info = 1 if name == ".symtab" else 0
    layout.append((name_offset, kind, 0, 0, offset, len(payload), link, info, 4, entsize))

while len(out) % 4:
    out += b"\0"
section_headers = len(out)
out += b"\0" * 40
for header in layout:
    out += struct.pack(">10I", *header)
ident = b"\x7fELF" + bytes([1, 2, 1, 0]) + b"\0" * 8
out[0:52] = ident + struct.pack(">HHIIIIIHHHHHH", 2, 20, 1, entry, 0, section_headers, 0, 52, 0, 0, 40,
                                len(layout) + 1, len(layout))
args.out.parent.mkdir(parents=True, exist_ok=True)
args.out.write_bytes(out)
# Ghidra's decompile script labels these in the existing (read-only) project too.
(args.out.parent / "data_symbols.txt").write_text(
    "".join(f"{address:08x} {name}\n" for address, name in sorted(objects.items())))
print(f"{args.out}: {len(sections)} sections, {len(symbols)} code symbols, {len(objects)} data symbols")
