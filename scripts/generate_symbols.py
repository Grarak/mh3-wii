#!/usr/bin/env python3
"""Generate MH3 function seeds from SEL text exports and verified PPC thunks.

Only executable sections 1 (.init) and 2 (.text) are used from the SEL.
Data exports are deliberately excluded. Additional stack-prologue seeds are
speculative inputs to the translator, not asserted symbol identifications.
"""
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOL_HASH = "db5c9939f677c33d15d446aa4ad19c28044d0b80c5b3cc75adc7f1340ecab162"


def main():
    dol = (ROOT / "Assets/DATA/sys/main.dol").read_bytes()
    if hashlib.sha256(dol).hexdigest() != DOL_HASH:
        raise ValueError("Symbol addresses require the verified RMHP08 DOL")
    inventory = json.loads((ROOT / "build/disc-inventory.json").read_text())
    text = [s for s in inventory["sections"] if s["kind"] == "text"]
    by_sel = {s["index"] + 1: s for s in text}
    symbols = {0x80006310: "__start", 0x804CB5A0: "OSExceptionInit",
               0x804CB930: "OSDefaultExceptionHandler"}
    for section in text:
        for pos in range(0, section["bytes"] - 8, 4):
            word, following = struct.unpack_from(">II", dol, section["file_offset"] + pos)
            # A standard non-leaf stack frame followed by mflr r0.
            if word & 0xFFFF8000 == 0x94218000 and following == 0x7C0802A6:
                address = int(section["address"], 16) + pos
                symbols.setdefault(address, f"func_{address:08X}")
    for symbol in inventory["static_exports"]["exports"]:
        section = by_sel.get(symbol["section"])
        if section is None:
            continue
        offset = symbol["offset"]
        if offset % 4 or offset >= section["bytes"]:
            raise ValueError(f"Invalid text export {symbol['name']}")
        symbols[int(section["address"], 16) + offset] = symbol["name"]
    # All 18 instructions and the final blr are checked, not just endpoints.
    families = [(0x804573BC, "_save_fpr_", 54, 8),
                (0x80457408, "_rest_fpr_", 50, 8),
                (0x80457454, "_save_gpr_", 36, 4),
                (0x804574A0, "_rest_gpr_", 32, 4)]
    for base, prefix, opcode, stride in families:
        section = next(s for s in text if int(s["address"], 16) <= base < int(s["address"], 16) + s["bytes"])
        offset = section["file_offset"] + base - int(section["address"], 16)
        for reg in range(14, 32):
            expected = (opcode << 26) | (reg << 21) | (11 << 16) | (((reg - 32) * stride) & 0xFFFF)
            if struct.unpack_from(">I", dol, offset + (reg - 14) * 4)[0] != expected:
                raise ValueError(f"Unrecognized thunk {prefix}{reg}")
            symbols[base + (reg - 14) * 4] = prefix + str(reg)
        if struct.unpack_from(">I", dol, offset + 18 * 4)[0] != 0x4E800020:
            raise ValueError(f"Unrecognized return in {prefix}")
    output = ROOT / "build/MAP.txt"
    output.write_text("".join(f"{a:08x} {n}\n" for a, n in sorted(symbols.items())))
    print(f"Function seeds: {len(symbols)} -> {output}")


if __name__ == "__main__":
    main()
