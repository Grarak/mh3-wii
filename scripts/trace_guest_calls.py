#!/usr/bin/env python3
"""Temporarily instrument translated collision callers and summarize their call edges.

The generated shards are ignored build products. `enable` backs up and edits
only shards containing TARGETS; `restore` puts their original bytes back.
Set MH3_CALL_TRACE to a CSV path while running an instrumented executable.
"""

import argparse
from bisect import bisect_right
from collections import Counter
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parent.parent
SHARDS = ROOT / "generated/build_shards/base_common"
MANIFEST = ROOT / "generated/build_shards/guest_trace_sources.cmake"
TARGETS = {
    0x80208590, 0x802094EC,              # player update and movement candidate
    0x802920F0, 0x8029513C, 0x802952F4, 0x802955A8,  # horizontal sweep candidate
    0x809F19CC, 0x809F1B48, 0x809F25B4,  # village movement / intersection caller
    0x802916A0, 0x802916C8, 0x80291D34,  # exported ground-hit path
    0x80291A60, 0x80291C20,              # callers observed via return addresses
    0x80295988, 0x802959C0, 0x802959FC,  # exported intersection wrappers / body
    0x802963CC, 0x802968D4, 0x80296A5C,  # ground bounds / grid / triangle
    0x80292A40, 0x80293568, 0x80293AE0,  # PAL wall-cheat candidate chain
}
HELPER = '// MH3_GUEST_CALL_TRACE_BEGIN\n#include "guest_call_trace.h"\n// MH3_GUEST_CALL_TRACE_END\n'
CALL = re.compile(
    r'(?m)^([ \t]*)(MKW_STATIC_TRANSLATED_CALL\((0x[0-9a-fA-F]+)u, [^\n]+\)|'
    r'InvokeDirectCpu<(0x[0-9a-fA-F]+)u>\(ctx\)|'
    r'InvokeIndirectCpu\(([^,\n]+), ctx\));$'
)


def instrument_function(body: str, address: int) -> tuple[str, int]:
    signature = f'extern "C" void func_{address:08X}(CpuContext* MKW_RESTRICT ctx)\n{{\n'
    if signature not in body:
        raise RuntimeError(f"Missing expected function signature {address:08X}")
    entry = f'    MKW_GUEST_TRACE_CALL(0, 0x{address:08X}u, ctx->lr - 4);\n'
    if address in (0x8029513C, 0x80293AE0):
        # Both PAL Gecko patches replace an lfs from r2 - 14184. Record r2
        # so the original and substitute small-data constants can be decoded.
        entry += f'    MKW_GUEST_TRACE_CALL(0xFFFFFFFDu, 0x{address:08X}u, ctx->gpr[2]);\n'
    body = body.replace(signature, signature + entry, 1)

    def replace(match: re.Match) -> str:
        indent, call, direct_macro, direct_cpp, indirect = match.groups()
        target = direct_macro or direct_cpp or indirect
        # r30 is the player object in 0x802094EC. This word is cleared by
        # 0x8029513C and ORed with per-hit flags by 0x802952F4.
        after = ''
        if address == 0x802094EC and direct_cpp == '0x8029513C':
            after = (' MKW_GUEST_TRACE_CALL(0xFFFFFFFEu, 0x8029513Cu, '
                     'MemoryInline::FlatRead32((ctx->gpr[30] + 868)));')
        return (f'{indent}do {{ MKW_GUEST_TRACE_CALL(0x{address:08X}u, {target}, '
                f'ctx->lr - 4); {call};{after} }} while (false);')

    return CALL.subn(replace, body)


def enable() -> None:
    remaining = set(TARGETS)
    traced_shards = []
    for shard in SHARDS.glob('*.cpp'):
        original = shard.read_text()
        addresses = [addr for addr in remaining if
                     f'extern "C" void func_{addr:08X}(CpuContext*' in original]
        if not addresses:
            continue
        backup = shard.with_suffix('.trace-original')
        if backup.exists():
            raise RuntimeError(f"Trace backup already exists: {backup}; restore first")
        updated = original
        calls = 0
        for addr in sorted(addresses, key=lambda a: updated.index(
                f'extern "C" void func_{a:08X}('), reverse=True):
            start = updated.index(f'extern "C" void func_{addr:08X}(')
            end = updated.find('\n#line 1 ', start)
            if end == -1:
                end = len(updated)
            piece, count = instrument_function(updated[start:end], addr)
            updated = updated[:start] + piece + updated[end:]
            calls += count
        first_line = updated.index('\n#line 1 ')
        updated = updated[:first_line] + '\n' + HELPER + updated[first_line:]
        backup.write_text(original)
        shard.write_text(updated)
        traced_shards.append(shard)
        remaining.difference_update(addresses)
        print(f"{shard.name}: {len(addresses)} functions, {calls} call sites")
    if remaining:
        raise RuntimeError(f"Trace targets not found: {[hex(a) for a in sorted(remaining)]}")
    MANIFEST.write_text('set(MH3_GUEST_TRACE_SOURCES\n' + ''.join(
        f'  "{shard}"\n' for shard in traced_shards) + ')\n')


def restore() -> None:
    for backup in SHARDS.glob('*.trace-original'):
        shard = backup.with_suffix('.cpp')
        backup.replace(shard)
        print(f"Restored {shard.name}")
    MANIFEST.unlink(missing_ok=True)


def summarize(path: Path) -> None:
    function_addresses = sorted(f['entryPoint'] for f in json.loads(
        (ROOT / 'generated/translation.json').read_text())['functions'])
    direct_edges: Counter[tuple[int, int]] = Counter()
    entry_edges: Counter[tuple[int, int]] = Counter()
    sweep_flags: Counter[int] = Counter()
    r2_values: Counter[tuple[int, int]] = Counter()
    malformed = 0
    for line in path.read_text().splitlines():
        parts = line.split(',')
        if len(parts) != 3 or any(len(part) != 8 for part in parts):
            malformed += 1
            continue
        source, target, _site = (int(part, 16) for part in parts)
        if source == 0xFFFFFFFE and target == 0x8029513C:
            sweep_flags[_site] += 1
            continue
        if source == 0xFFFFFFFD:
            r2_values[target, _site] += 1
            continue
        if source:
            direct_edges[source, target] += 1
        else:
            index = bisect_right(function_addresses, _site) - 1
            if index >= 0:
                entry_edges[function_addresses[index], target] += 1
    edges = direct_edges | entry_edges
    names = {}
    symbols = (ROOT / 'generated/guest_symbol_table.cpp').read_text()
    addresses = re.findall(r'0x([0-9A-Fa-f]+)u', symbols.split('kGuestMapSymbolNames[] =')[0])
    labels = re.findall(r'"([^"]+)"', symbols.split('kGuestMapSymbolNames[] = {', 1)[1])
    names.update(zip((int(address, 16) for address in addresses), labels))
    for line in (ROOT / 'symbols.txt').read_text().splitlines():
        match = re.match(r'^([0-9A-Fa-f]{8})\s+(\S+)', line)
        if match:
            names[int(match.group(1), 16)] = match.group(2)

    def label(address: int) -> str:
        name = names.get(address, f'func_{address:08X}')
        return f'{address:08X} {name}'

    print(f"{sum(direct_edges.values()) + sum(entry_edges.values())} events, "
          f"{len(edges)} distinct edges, {malformed} malformed lines")
    for (source, target), count in edges.most_common():
        print(f'{count:8d}  {label(source)} -> {label(target)}')
    if sweep_flags:
        print('Sweep flag word at player +868:')
        for value, count in sweep_flags.most_common():
            print(f'{count:8d}  0x{value:08x}')
    if r2_values:
        print('Small-data base r2 at PAL Gecko patch sites:')
        for (target, value), count in r2_values.most_common():
            print(f'{count:8d}  0x{target:08x} r2=0x{value:08x}')


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('action', choices=['enable', 'restore', 'summarize'])
parser.add_argument('trace', nargs='?', type=Path, help='CSV file for summarize')
args = parser.parse_args()
if args.action == 'enable':
    enable()
elif args.action == 'restore':
    restore()
elif args.trace:
    summarize(args.trace)
else:
    parser.error('summarize requires a CSV path')
