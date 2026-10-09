#!/usr/bin/env python3
"""Generate MH3 function seeds from exports, tables, and PPC entry patterns.

Only executable sections 1 (.init) and 2 (.text) are used from the SEL.
Data exports are deliberately excluded. Stack prologues, return-boundary pointers, adjustment thunks, and recognized
vtable shapes provide speculative inputs, not asserted symbol identifications.
"""
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOL_HASH = "db5c9939f677c33d15d446aa4ad19c28044d0b80c5b3cc75adc7f1340ecab162"


def discover_indirect_pointer_seeds(dol, sections):
    """Speculative entries: return boundaries and two-instruction this-adjustment thunks.

    This does not assign SDK names or claim that arbitrary data words are code.
    The strict translator must still decode and validate each candidate.
    """
    text = [s for s in sections if s["kind"] == "text"]
    candidates = set()
    for section in sections:
        if section["kind"] != "data":
            continue
        for pos in range(section["file_offset"],
                         section["file_offset"] + section["bytes"] - 3, 4):
            address = struct.unpack_from(">I", dol, pos)[0]
            if address % 4:
                continue
            for code in text:
                base = int(code["address"], 16)
                if base <= address < base + code["bytes"]:
                    offset = code["file_offset"] + address - base
                    if address >= base + 4 and struct.unpack_from(
                            ">I", dol, offset - 4)[0] in (0x4E800020, 0x4E800420):
                        candidates.add(address)
                    # C++ multiple-inheritance thunks adjust the this pointer
                    # and tail-branch to the implementation. Validate both words
                    # and the resolved branch destination, not the prefix alone.
                    if address + 8 <= base + code["bytes"]:
                        first, branch = struct.unpack_from(">II", dol, offset)
                        if first & 0xFFFF0000 == 0x38630000 and branch & 0xFC000003 == 0x48000000:
                            delta = ((branch & 0x03FFFFFC) ^ 0x02000000) - 0x02000000
                            target = (address + 4 + delta) & 0xFFFFFFFF
                            if any(int(t["address"], 16) <= target <
                                   int(t["address"], 16) + t["bytes"] for t in text):
                                candidates.add(address)
                    break
    return candidates


def discover_code_constant_seeds(dol, sections):
    """Speculative entries: text addresses the code builds with lis + addi/ori
    (callbacks handed to other functions) that start after a blr or bctr.

    The online code passes such leaf callbacks (0x803CF830, a register
    shuffle that tail-calls) and reaches them only through the pointer. As for
    data pointers, the return boundary is a heuristic and the strict
    translator validates each candidate.
    """
    text = [s for s in sections if s["kind"] == "text"]
    candidates = set()

    def boundary(address):
        for code in text:
            base = int(code["address"], 16)
            if base + 4 <= address < base + code["bytes"]:
                return struct.unpack_from(">I", dol, code["file_offset"] + address - base - 4)[0] in (0x4E800020, 0x4E800420)
        return False

    for code in text:
        words = struct.unpack_from(f">{code['bytes'] // 4}I", dol, code["file_offset"])
        for i, word in enumerate(words):
            # lis rD, high (addis rD, 0, high)
            if word >> 26 != 15 or (word >> 16) & 31:
                continue
            target, high = (word >> 21) & 31, word & 0xFFFF
            # The low half within the next three instructions: addi/ori rX, rD, low.
            for follow in words[i + 1:i + 4]:
                opcode, source, low = follow >> 26, (follow >> 16) & 31, follow & 0xFFFF
                if opcode in (14, 24) and source == target:
                    if opcode == 14:
                        address = ((high << 16) + low - (0x10000 if low & 0x8000 else 0)) & 0xFFFFFFFF
                    else:
                        address = (high << 16) | low
                    if address % 4 == 0 and boundary(address):
                        candidates.add(address)
                    break
                # Stop at another write of rD (stores, which read it, go on).
                if (follow >> 21) & 31 == target and opcode not in (32, 34, 36, 37, 40, 44, 48, 52):
                    break
    return candidates


def discover_vtable_pointer_seeds(dol, sections, known_entries):
    """Speculative vtables with a zero header and a recognized first method."""
    text = [s for s in sections if s["kind"] == "text"]
    def executable(address):
        return address % 4 == 0 and any(int(s["address"], 16) <= address <
                int(s["address"], 16) + s["bytes"] for s in text)
    candidates = set()
    for section in sections:
        if section["kind"] != "data":
            continue
        words = struct.unpack_from(">" + str(section["bytes"] // 4) + "I",
                                   dol, section["file_offset"])
        for start in range(2, len(words) - 1):
            if words[start - 2:start] != (0, 0) or words[start] not in known_entries:
                continue
            end = start
            while end < len(words) and executable(words[end]):
                end += 1
            if end - start >= 2:
                candidates.update(words[start:end])
    return candidates


def main():
    dol = (ROOT / "Assets/DATA/sys/main.dol").read_bytes()
    if hashlib.sha256(dol).hexdigest() != DOL_HASH:
        raise ValueError("Symbol addresses require the verified RMHP08 DOL")
    inventory = json.loads((ROOT / "build/disc-inventory.json").read_text())
    text = [s for s in inventory["sections"] if s["kind"] == "text"]
    by_sel = {s["index"] + 1: s for s in text}
    symbols = {0x80006310: "__start", 0x804CB5A0: "OSExceptionInit",
               0x804CB930: "OSDefaultExceptionHandler"}
    # Startup iterates these null-terminated tables, including tail-call
    # constructor stubs that have no standard stack prologue.
    for base, label in [(0x8056FAA0, "ctor"), (0x8056FC20, "dtor")]:
        section = next(s for s in inventory["sections"] if int(s["address"], 16) == base)
        terminated = False
        for offset in range(0, section["bytes"], 4):
            address = struct.unpack_from(">I", dol, section["file_offset"] + offset)[0]
            if address == 0:
                terminated = True
                continue
            if terminated or address % 4 or not any(
                int(s["address"], 16) <= address < int(s["address"], 16) + s["bytes"] for s in text
            ):
                raise ValueError(f"Invalid {label} table entry {address:08x}")
            symbols.setdefault(address, f"{label}_{address:08X}")
        if not terminated:
            raise ValueError(f"Unterminated {label} table")
    # Verified virtual-call tables used by save-selection initialization. The
    # leaf getters/setters and tail-call entries have no stack prologue and are
    # absent from SEL exports. Bounds exclude the vtable's zero header/trailer.
    # Runtime dispatch confirmed font slot 0x8063053C and UI slot 0x80653E20.
    for start, end, label in [(0x80630520, 0x80630578, "FontVtable"),
                              (0x80653DF0, 0x80653EBC, "SaveUiVtable"),
                              (0x80659EC0, 0x8065A008, "SaveUiDerivedVtable")]:
        section = next(s for s in inventory["sections"]
                       if int(s["address"], 16) <= start and
                       end <= int(s["address"], 16) + s["bytes"])
        offset = section["file_offset"] + start - int(section["address"], 16)
        for pos in range(0, end - start, 4):
            address = struct.unpack_from(">I", dol, offset + pos)[0]
            if address % 4 or not any(int(s["address"], 16) <= address <
                    int(s["address"], 16) + s["bytes"] for s in text):
                raise ValueError(f"Invalid {label} function pointer {address:08x}")
            symbols.setdefault(address, f"{label}_{address:08X}")
    # Labels may also refer to the hash-pinned boot RSOs added to the analysis
    # image by prepare_rso_translation.py. Validate their executable ranges.
    from prepare_rso_translation import MODULES, Rso
    boot_text = []
    for module, base, digest in MODULES:
        data = (ROOT / f"Assets/DATA/files/01/{module}_data.rso").read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"Unverified boot RSO: {module}")
        offset, size = Rso(data).sections[1]
        boot_text.append((base + offset, base + offset + size))
    for line in (ROOT / "symbols.txt").read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        address, name = line.split(maxsplit=1)
        address = int(address, 16)
        if address % 4 or not (any(int(s["address"], 16) <= address < int(s["address"], 16) + s["bytes"] for s in text)
                               or any(start <= address < end for start, end in boot_text)):
            raise ValueError(f"Verified function lies outside executable sections: {line}")
        symbols[address] = name
    for section in text:
        for pos in range(0, section["bytes"] - 8, 4):
            word, following = struct.unpack_from(">II", dol, section["file_offset"] + pos)
            # A standard non-leaf stack frame followed by mflr r0.
            if word & 0xFFFF8000 == 0x94218000 and following == 0x7C0802A6:
                address = int(section["address"], 16) + pos
                symbols.setdefault(address, f"func_{address:08X}")
    for address in discover_indirect_pointer_seeds(dol, inventory["sections"]):
        symbols.setdefault(address, f"data_indirect_seed_{address:08X}")
    for address in discover_code_constant_seeds(dol, inventory["sections"]):
        symbols.setdefault(address, f"code_constant_seed_{address:08X}")
    for symbol in inventory["static_exports"]["exports"]:
        section = by_sel.get(symbol["section"])
        if section is None:
            continue
        offset = symbol["offset"]
        if offset % 4 or offset >= section["bytes"]:
            raise ValueError(f"Invalid text export {symbol['name']}")
        symbols[int(section["address"], 16) + offset] = symbol["name"]
    for address in discover_vtable_pointer_seeds(dol, inventory["sections"], set(symbols)):
        symbols.setdefault(address, f"vtable_seed_{address:08X}")
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
