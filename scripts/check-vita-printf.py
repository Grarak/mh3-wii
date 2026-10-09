#!/usr/bin/env python3
"""Reject C99-only printf length modifiers (%zu, %jd, %td) in runtime code.

VitaSDK's newlib is built without _WANT_IO_C99_FORMATS: "%zu" prints a
literal "z" and leaves its argument for the next conversion, so a "%s"
after it reads a size as a pointer and faults (a texture conversion print
hung the devkit that way). Cast to unsigned long and use %lu instead.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
TREES = ["runtime/src", "runtime/vita", "runtime/include", "runtime/native", "aurora-main/lib", "aurora-main/include"]
FORMAT = re.compile(r'%[-+ #0-9.*]*[zjt][diouxXn]')
STRING = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def main():
    bad = []

    for tree in TREES:
        for path in sorted((ROOT / tree).rglob("*")):
            if path.suffix not in {".c", ".cc", ".cpp", ".h", ".hpp", ".inc"}:
                continue

            for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
                if any(FORMAT.search(text) for text in STRING.findall(line)):
                    bad.append(f"{path.relative_to(ROOT)}:{number}: {line.strip()}")

    for line in bad:
        print(line)

    if bad:
        print("C99 printf length modifiers print garbage or fault on the Vita; use %lu with static_cast<unsigned long>")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
