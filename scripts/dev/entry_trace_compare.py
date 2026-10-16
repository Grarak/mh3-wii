#!/usr/bin/env python3
"""Compares two MH3_ENTRY_TRACE files (runtime/src/guest_entry_trace.cpp):
which translated functions one run entered and the other did not.

  entry_trace_compare.py BEFORE AFTER [--native ROOT]... [--range LO-HI]...

BEFORE and AFTER are traces of the same scenario, typically a build before
a group of native ports and one with them. A natively registered function
runs no translated body, so the functions registered in any --native tree
(REGISTER_NATIVE_FUNCTION[_AS] in ROOT/runtime/src) are left out of both
sides. --range limits the report to guest address ranges (hex, inclusive);
the labels come from symbols.txt and build/MAP.txt of the first --native
tree. Counts are printed for a sense of scale only: two runs never spend
the same time in each state.
"""
import argparse
import pathlib
import re
import sys

REGISTRATION = re.compile(r'REGISTER_NATIVE_FUNCTION(?:_AS)?\(\s*0x([0-9A-Fa-f]{8})')


def read_trace(path):
    entries = {}

    for line in open(path):
        if line.startswith('#') or not line.strip():
            continue

        address, count, _first = line.split()
        entries[int(address, 16)] = int(count)

    return entries


def native_addresses(root):
    found = set()

    for source in (pathlib.Path(root) / 'runtime' / 'src').rglob('*'):
        if source.suffix in ('.cpp', '.c', '.h', '.inc'):
            found.update(int(m, 16) for m in REGISTRATION.findall(source.read_text(errors='replace')))

    return found


def labels(root):
    names = {}

    for name in ('build/MAP.txt', 'symbols.txt'):
        path = pathlib.Path(root) / name

        if not path.exists():
            continue

        for line in open(path, errors='replace'):
            parts = line.split()

            if len(parts) >= 2 and re.fullmatch(r'(0x)?[0-9A-Fa-f]{8}', parts[0]):
                names[int(parts[0], 16)] = parts[1]

    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('before')
    parser.add_argument('after')
    parser.add_argument('--native', action='append', default=[], metavar='ROOT')
    parser.add_argument('--range', action='append', default=[], metavar='LO-HI')
    args = parser.parse_args()
    before, after = read_trace(args.before), read_trace(args.after)
    skip = set()

    for root in args.native:
        skip |= native_addresses(root)

    ranges = [tuple(int(v, 16) for v in r.split('-')) for r in args.range]
    names = labels(args.native[0]) if args.native else {}

    def wanted(address):
        return address not in skip and (not ranges or any(lo <= address <= hi for lo, hi in ranges))

    only_before = sorted(a for a in before if a not in after and wanted(a))
    only_after = sorted(a for a in after if a not in before and wanted(a))
    both = sum(1 for a in before if a in after and wanted(a))
    print(f'# {both} functions entered in both, {len(only_before)} only before, {len(only_after)} only after '
          f'({len(skip)} native addresses left out)')

    for title, side, other in (('only before', only_before, before), ('only after', only_after, after)):
        print(f'# {title}')

        for address in side:
            print(f'{address:08x} {other[address]:>10} {names.get(address, "")}')

    return 1 if only_before or only_after else 0


if __name__ == '__main__':
    sys.exit(main())
