#!/usr/bin/env python3
"""Check that the embedded GXM shader bank holds every native shader variant a
host run used (docs/optimizations/rendering/offline-shader-bank.md).

Development builds print one line per native shader program they create:

  MH3 shader bank key v=VVVV f=FFFF a1=AAAA vertex-name+fragment-name

v and f are the variant's Vita stage keys; a1 is its fragment with the
target-alpha-one certificate flipped (hosts never certify A=1, the Vita may).
Reports every variant and stage the bank lacks and exits 1 on any miss.

  scripts/vita-shader-bank-audit.py --bank build/vita/gxm-shader-bank/shader_bank_tables.cpp logs/runs/alat.txt ...
"""
import argparse
import re
import sys
from pathlib import Path

LINE = re.compile(r'MH3 shader bank key v=([0-9a-f]{16}) f=([0-9a-f]{16}) a1=([0-9a-f]{16}) (\S+)\+(\S+)')
TABLE = re.compile(r'const Program (Vertex|Fragment|FragmentScalar)Programs\[\] = \{(.*?)\};', re.S)
ROW = re.compile(r'\{0x([0-9a-f]{16})ull,')


def bank_keys(path):
    keys = {'v': set(), 'f': set(), 'r': set()}
    text = path.read_text()
    if path.suffix == '.tsv':
        for line in text.splitlines():
            if line and not line.startswith('#'):
                key, stage = line.split('\t')[:2]
                keys[stage].add(key)
    else:
        for name, body in TABLE.findall(text):
            keys[{'Vertex': 'v', 'Fragment': 'f', 'FragmentScalar': 'r'}[name]].update(ROW.findall(body))
    if not keys['v'] or not keys['f']:
        raise SystemExit(f'No shader bank keys in {path}')
    return keys


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bank', type=Path, required=True, help='shader_bank_tables.cpp of a Vita build, or a manifest.tsv')
    parser.add_argument('logs', type=Path, nargs='+', help='development-build game logs')
    args = parser.parse_args()
    keys = bank_keys(args.bank)
    variants = {}
    for log in args.logs:
        for match in LINE.finditer(log.read_text(errors='replace')):
            variants.setdefault(match.groups(), set()).add(log.name)
    if not variants:
        raise SystemExit('No "MH3 shader bank key" lines; run a development build')
    misses = []
    for (v, f, a1, vertex, fragment), logs in sorted(variants.items(), key=lambda item: item[0][3:]):
        for stage, key, label in (('v', v, vertex), ('f', f, fragment), ('f', a1, fragment + ' (A=1 flipped)')):
            if key not in keys[stage]:
                misses.append(f'MISS {key}_{stage} {label} ({", ".join(sorted(logs))})')
    vertex_keys = {k[0] for k in variants}
    fragment_keys = {k[1] for k in variants}
    either_alpha = fragment_keys | {k[2] for k in variants}
    print(f'{len(variants)} variants used: {len(vertex_keys)} vertex and {len(fragment_keys)} fragment stages '
          f'({len(either_alpha)} with the A=1 forms); bank holds {len(keys["v"])} vertex, {len(keys["f"])} fragment, {len(keys["r"])} scalar')
    for name in sorted({f'{k[3]}+{k[4]}' for k in variants}):
        print(f'  {name}')
    if misses:
        print('\n'.join(misses))
        print(f'SHADER BANK AUDIT FAILED: {len(misses)} stages missing', file=sys.stderr)
        return 1
    print('shader bank audit: every used stage is banked')
    return 0


if __name__ == '__main__':
    sys.exit(main())
