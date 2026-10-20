#!/usr/bin/env python3
"""Decodes an action dispatcher's jump table from main.dol: for each case,
the constant arguments it loads (li r4..) and the function it branches to.
The hunter's weapon dispatchers (runtime/src/weapon_actions.cpp) are such
switches on the action number (PLW +12); so are many of the game's other
action steps.

  action-dispatch.py [--dol PATH] ADDR...

ADDR is the dispatcher's entry (hex). It finds `lis/addi` (the table),
`cmplwi` (the last case) and the `bctr`; a case that does more than load
arguments and branch is printed with its other instruction words.
"""
import argparse
import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parents[2]


class Dol:
    def __init__(self, path):
        self.data = path.read_bytes()
        self.sections = list(zip(struct.unpack_from('>18I', self.data, 0), struct.unpack_from('>18I', self.data, 0x48), struct.unpack_from('>18I', self.data, 0x90)))

    def word(self, address):
        for offset, start, size in self.sections:
            if start <= address < start + size:
                return struct.unpack_from('>I', self.data, offset + address - start)[0]

        raise KeyError(f'{address:08x} is not in the executable')


def signed16(value):
    return value - 0x10000 if value & 0x8000 else value


def branch_target(address, word):
    offset = word & 0x3FFFFFC
    return address + (offset - 0x4000000 if offset & 0x2000000 else offset)


def decode_case(dol, address):
    """The case's li arguments, its branch target and kind (b: tail call,
    bl: call, then the next instruction), and any other instruction words."""
    arguments, other = {}, []

    for k in range(12):
        at = address + 4 * k
        word = dol.word(at)
        opcode, rt, ra = word >> 26, (word >> 21) & 31, (word >> 16) & 31

        if opcode == 14 and ra == 0:
            arguments[rt] = signed16(word & 0xFFFF)
        elif word & 0xFC0007FE == 0x7C000378 and rt == (word >> 11) & 31:
            other.append(f'mr r{ra},r{rt}')
        elif opcode == 18:
            if word & 1:
                after = dol.word(at + 4)
                follow = f'then b {branch_target(at + 4, after):08x}' if after >> 26 == 18 else f'then {after:08x}'
                return arguments, branch_target(at, word), 'bl', other + [follow]

            return arguments, branch_target(at, word), 'b', other
        else:
            other.append(f'{word:08x}')

    return arguments, None, '?', other


def find_table(dol, entry):
    high = low = last = None

    for k in range(64):
        word = dol.word(entry + 4 * k)
        opcode = word >> 26

        if opcode == 15:
            high = (word & 0xFFFF) << 16
        elif opcode == 14 and high is not None and (word >> 16) & 31 == (word >> 21) & 31:
            low = signed16(word & 0xFFFF)
        elif opcode == 10:
            last = word & 0xFFFF
        elif word == 0x4E800420:
            return (high + low) & 0xFFFFFFFF, last, entry + 4 * k

    raise ValueError(f'{entry:08x}: no bctr in its first 64 instructions')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dol', type=pathlib.Path, default=ROOT / 'build/linux-arm64-dev/disc/sys/main.dol')
    parser.add_argument('entries', nargs='+')
    args = parser.parse_args()
    dol = Dol(args.dol)

    for text in args.entries:
        entry = int(text, 16)
        table, last, bctr = find_table(dol, entry)
        print(f'## {entry:08x}: table {table:08x}, actions 0-{last} (bctr at {bctr:08x})')

        for action in range(last + 1):
            case = dol.word(table + 4 * action)
            arguments, target, kind, other = decode_case(dol, case)
            registers = ' '.join(f'r{r}={v}' for r, v in sorted(arguments.items()))
            print(f'{action:3} {case:08x} {kind} {target and f"{target:08x}"} {registers} {" ".join(other)}'.rstrip())


if __name__ == '__main__':
    main()
