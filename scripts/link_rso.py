#!/usr/bin/env python3
"""Offline RSO section linker for static translation (never used at runtime).

Header layout: Dolphin Core/Debugger/RSO.h. Relocation behavior verified against
RMHP08's linker at 0x804DB0A4. Only implemented relocation kinds are accepted.
The original guest loader remains responsible for loading/linking live modules.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOL_HASH = 'db5c9939f677c33d15d446aa4ad19c28044d0b80c5b3cc75adc7f1340ecab162'
# RMHP08's SEL initializer (0x804DA544), independently checked against its
# linked section table. Includes BSS/small BSS, absent from DOL file sections.
SEL_BASES = [0, 0x80004000, 0x8003F260, 0, 0, 0x8056FC40, 0x8057CFE0,
             0x8065A980, 0x807932E0, 0x80797F60, 0, 0x80796C20, 0x8079FCA0]


def span(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError(f'RSO range outside file: {offset:#x}+{size:#x}')
    return memoryview(data)[offset:offset + size]


def u32(data, offset):
    return struct.unpack('>I', span(data, offset, 4))[0]


def records(data, offset, size, stride):
    span(data, offset, size)
    if size % stride:
        raise ValueError('Misaligned RSO table size')
    return [struct.unpack('>' + 'I' * (stride // 4), span(data, p, stride))
            for p in range(offset, offset + size, stride)]


def name(data, offset):
    span(data, offset, 1)
    end = data.find(b'\0', offset)
    if end < 0:
        raise ValueError('Unterminated RSO name')
    return data[offset:end].decode('ascii')


class Rso:
    def __init__(self, data):
        self.data = bytes(data)
        span(data, 0, 0x58)
        if u32(data, 0x18) != 1:
            raise ValueError('Unsupported RSO version')
        count = u32(data, 8)
        if not 1 <= count <= 256:
            raise ValueError('Invalid RSO section count')
        self.sections = records(data, u32(data, 12), count * 8, 8)
        for offset, size in self.sections:
            if offset:
                span(data, offset, size)
        self.exports = [(name(self.data, u32(data, 0x48) + n), off, sec)
                        for n, off, sec, _ in records(data, u32(data, 0x40), u32(data, 0x44), 16)]
        self.imports = [name(self.data, u32(data, 0x54) + n)
                        for n, _, _ in records(data, u32(data, 0x4C), u32(data, 0x50), 12)]
        self.internal = records(data, u32(data, 0x30), u32(data, 0x34), 12)
        self.external = records(data, u32(data, 0x38), u32(data, 0x3C), 12)

    def link(self, base, imports):
        if base % 32 or not 0x80000000 <= base < 0x81800000:
            raise ValueError('RSO base must be aligned within MEM1')
        bss = (base + len(self.data) + 31) & ~31
        if bss + u32(self.data, 0x1C) > 0x81800000:
            raise ValueError('RSO image exceeds MEM1')
        addresses, bss_used = [], 0
        for offset, size in self.sections:
            if offset:
                addresses.append(base + offset)
            elif size:
                addresses.append(bss + bss_used)
                bss_used += size
            else:
                addresses.append(0)
        if bss_used != u32(self.data, 0x1C):
            raise ValueError('RSO BSS section sizes disagree with header')
        image = bytearray(self.data)
        for external, entries in ((False, self.internal), (True, self.external)):
            for offset, info, addend in entries:
                index, kind = info >> 8, info & 255
                if kind == 0:
                    continue
                if external:
                    if index >= len(self.imports):
                        raise ValueError('RSO import index outside table')
                    symbol = self.imports[index]
                    if symbol not in imports:
                        raise ValueError(f'Unresolved RSO import: {symbol}')
                    value = imports[symbol]
                else:
                    if index >= len(addresses) or not addresses[index]:
                        raise ValueError('RSO relocation references absent section')
                    value = addresses[index]
                relocate(image, offset, kind, (value + addend) & 0xFFFFFFFF, base + offset)
        return image, addresses


def relocate(image, offset, kind, value, address):
    size = 2 if kind in (3, 4, 5, 6) else 4
    span(image, offset, size)
    if offset % size:
        raise ValueError('Misaligned RSO relocation')
    if kind == 1:
        struct.pack_into('>I', image, offset, value)
    elif kind in (3, 4, 5, 6):
        value = value if kind in (3, 4) else (value + (0x8000 if kind == 6 else 0)) >> 16
        struct.pack_into('>H', image, offset, value & 0xFFFF)
    elif kind == 10:
        delta = value - address
        if delta % 4 or not -0x2000000 <= delta < 0x2000000:
            raise ValueError('RSO relative branch exceeds REL24 range')
        word = u32(image, offset)
        struct.pack_into('>I', image, offset, (word & 0xFC000003) | (delta & 0x3FFFFFC))
    else:
        raise ValueError(f'Unsupported RSO relocation kind {kind}')


def static_exports():
    dol = (ROOT / 'Assets/DATA/sys/main.dol').read_bytes()
    if hashlib.sha256(dol).hexdigest() != DOL_HASH:
        raise ValueError('SEL section mapping requires the verified RMHP08 DOL')
    sel = Rso((ROOT / 'Assets/DATA/files/mh3.sel').read_bytes())
    result = {}
    for symbol, offset, section in sel.exports:
        if section == 0xFFF1:  # SHN_ABS, including _SDA_BASE_ / _SDA2_BASE_
            result[symbol] = offset
            continue
        if section >= len(SEL_BASES) or not SEL_BASES[section]:
            raise ValueError(f'Unknown SEL export section: {symbol}: {section}')
        if symbol in result:
            raise ValueError(f'Duplicate SEL export: {symbol}')
        result[symbol] = SEL_BASES[section] + offset
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', type=Path)
    parser.add_argument('--base', required=True, type=lambda s: int(s, 0))
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--verify-mem1', type=Path)
    args = parser.parse_args()
    rso = Rso(args.module.read_bytes())
    image, addresses = rso.link(args.base, static_exports())
    if args.verify_mem1:
        memory = args.verify_mem1.read_bytes()
        if len(memory) != 0x1800000:
            raise ValueError('Expected a complete 24 MiB MEM1 snapshot')
        for (offset, size), address in zip(rso.sections, addresses):
            if not offset or not size:
                continue
            actual = span(memory, address - 0x80000000, size)
            expected = image[offset:offset + size]
            if actual != expected:
                mismatch = next(i for i, (a, b) in enumerate(zip(actual, expected)) if a != b)
                raise ValueError(f'Guest linker mismatch at {address + mismatch:#010x}')
        print('All initialized sections match the original guest linker byte for byte')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    manifest = {'source_sha256': hashlib.sha256(rso.data).hexdigest(), 'base': args.base,
                'sections': [{'address': a, 'file_offset': o, 'size': s}
                             for (o, s), a in zip(rso.sections, addresses)],
                'prolog': addresses[rso.data[0x20]] + u32(rso.data, 0x24)}
    args.output.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Linked {args.module.name}: {len(rso.internal)} internal and {len(rso.external)} external relocations')


if __name__ == '__main__':
    main()
