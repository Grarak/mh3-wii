#!/usr/bin/env python3
"""Compare the RMHE08 Walk through Walls patch with its published RMHP08 port.

Extract the user's USA RVZ with `tools/nodtool extract -q INPUT build/usa-extracted`
first. This script only reads the two main.dol files; it never applies cheats.
"""

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCHES = (
    (0x80293A7C, 0x80293AE0, 0xC002FDD0),
    (0x80295168, 0x802951CC, 0xC022FDEC),
)
USA_DATA_WRITE = 0x8079D870
PUBLISHED_PAL_DATA_WRITE = 0x8079D8D4


class Dol:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        self.sections = []
        for index in range(18):
            offset, address, size = (struct.unpack_from(">I", self.data, base + 4 * index)[0]
                                     for base in (0, 0x48, 0x90))
            if size:
                self.sections.append((offset, address, size))
        # Both discs put .sdata2 at DOL section 14, with _SDA2_BASE_ at +0x8000.
        sdata2 = struct.unpack_from(">I", self.data, 0x48 + 4 * 14)[0]
        sdata2_size = struct.unpack_from(">I", self.data, 0x90 + 4 * 14)[0]
        if not sdata2_size:
            raise ValueError(f"No section 14 (.sdata2): {path}")
        self.r2 = sdata2 + 0x8000

    def word(self, address: int) -> int | None:
        for offset, start, size in self.sections:
            if start <= address <= start + size - 4:
                return struct.unpack_from(">I", self.data, offset + address - start)[0]
        return None


def signed16(value: int) -> int:
    return value - 0x10000 if value & 0x8000 else value


def lfs(word: int) -> tuple[int, int, int]:
    if word >> 26 != 48:
        raise ValueError(f"Expected lfs, got 0x{word:08X}")
    return (word >> 21) & 31, (word >> 16) & 31, signed16(word & 0xFFFF)


def describe(dol: Dol, address: int, replacement: int) -> int:
    original = dol.word(address)
    if original is None:
        raise ValueError(f"0x{address:08X} is not mapped by {dol.path}")
    old_dest, old_base, old_disp = lfs(original)
    new_dest, new_base, new_disp = lfs(replacement)
    if old_base != 2 or new_base != 2 or old_dest != new_dest:
        raise ValueError(f"Unexpected patch shape at 0x{address:08X}")
    print(f"  0x{address:08X}: 0x{original:08X} lfs f{old_dest},{old_disp}(r2)"
          f" -> 0x{replacement:08X} lfs f{new_dest},{new_disp}(r2)")
    print(f"      original reads 0x{dol.r2 + old_disp:08X}; patch reads"
          f" 0x{dol.r2 + new_disp:08X}")
    return dol.r2 + new_disp


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--usa-dol", type=Path,
                        default=ROOT / "build/usa-extracted/sys/main.dol")
    parser.add_argument("--pal-dol", type=Path,
                        default=ROOT / "Assets/DATA/sys/main.dol")
    args = parser.parse_args()
    usa, pal = Dol(args.usa_dol), Dol(args.pal_dol)
    print(f"USA r2=0x{usa.r2:08X}; PAL r2=0x{pal.r2:08X}")
    for usa_address, pal_address, replacement in PATCHES:
        print("USA:")
        usa_target = describe(usa, usa_address, replacement)
        print("PAL:")
        pal_target = describe(pal, pal_address, replacement)
        if usa_address == PATCHES[0][0]:
            if usa_target != USA_DATA_WRITE:
                raise ValueError("USA data write does not match the patched lfs")
            print(f"  Corresponding PAL data write: 0x{pal_target:08X}")
    published = pal.word(PUBLISHED_PAL_DATA_WRITE)
    if published is None:
        raise ValueError("Published PAL data address is not in the DOL")
    print(f"Published PAL write 0x{PUBLISHED_PAL_DATA_WRITE:08X} holds"
          f" 0x{published:08X} ({struct.unpack('>f', struct.pack('>I', published))[0]});"
          " it does not address the patched lfs operand.")


if __name__ == "__main__":
    main()
