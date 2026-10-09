#!/usr/bin/env python3
"""Inventory extracted MH3 binaries without modifying the disc or executables.

RSO layout reference:
https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/Debugger/RSO.h
Symbol offsets remain section-relative; this does not invent load addresses.
"""
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "Assets/DATA"


def u32(data, offset):
    return struct.unpack_from(">I", data, offset)[0]


def string(data, offset):
    return data[offset:data.index(0, offset)].decode("utf-8", errors="replace")


def module(path):
    data = path.read_bytes()
    sections = []
    for i in range(u32(data, 8)):
        offset, size = struct.unpack_from(">II", data, u32(data, 12) + i * 8)
        sections.append({"index": i, "offset_raw": offset, "size": size})
    exports = []
    for pos in range(u32(data, 64), u32(data, 64) + u32(data, 68), 16):
        name, offset, section, symbol_hash = struct.unpack_from(">4I", data, pos)
        exports.append({"name": string(data, u32(data, 72) + name),
                        "offset": offset, "section": section, "hash": symbol_hash})
    imports = []
    for pos in range(u32(data, 76), u32(data, 76) + u32(data, 80), 12):
        name, offset, entry = struct.unpack_from(">3I", data, pos)
        imports.append({"name": string(data, u32(data, 84) + name),
                        "offset": offset, "entry": entry})
    return {"path": str(path.relative_to(ROOT)), "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "sections": sections, "bss_bytes": u32(data, 28),
            "internal_relocations": u32(data, 52) // 12,
            "external_relocations": u32(data, 60) // 12,
            "exports": exports, "imports": imports}


def main():
    dol = (DATA / "sys/main.dol").read_bytes()
    sections = []
    low_branches = []
    for i in range(18):
        offset = u32(dol, i * 4)
        address = u32(dol, 0x48 + i * 4)
        size = u32(dol, 0x90 + i * 4)
        if not size:
            continue
        sections.append({"index": i, "kind": "text" if i < 7 else "data",
                         "file_offset": offset, "address": hex(address), "bytes": size})
        if i >= 7:
            continue
        for pos in range(0, size, 4):
            instruction = u32(dol, offset + pos)
            if instruction >> 26 != 18:
                continue
            displacement = instruction & 0x3FFFFFC
            if displacement & 0x2000000:
                displacement -= 0x4000000
            target = (displacement if instruction & 2 else address + pos + displacement) & 0xFFFFFFFF
            if target < 0x80000000:
                low_branches.append({"address": hex(address + pos),
                                     "instruction": hex(instruction), "target": hex(target)})
    modules = [module(p) for p in sorted((DATA / "files").rglob("*.rso"))]
    sel = module(DATA / "files/mh3.sel")
    result = {
        "game_id": (DATA / "sys/boot.bin").read_bytes()[:6].decode("ascii"),
        "dol_sha256": hashlib.sha256(dol).hexdigest(),
        "entry_point": hex(u32(dol, 0xE0)), "sections": sections,
        "bss_address": hex(u32(dol, 0xD8)), "bss_bytes": u32(dol, 0xDC),
        "low_address_direct_branches": low_branches,
        "static_exports": sel, "modules": modules,
    }
    output = ROOT / "build/disc-inventory.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"{result['game_id']}: entry {result['entry_point']}, {len(modules)} RSO modules, "
          f"{len(sel['exports'])} SEL exports")
    print(f"Inventory: {output}")


if __name__ == "__main__":
    main()
