#!/usr/bin/env python3
"""Prepare verified fixed-address boot RSOs for analysis, never runtime data init."""
import hashlib
import struct
from link_rso import ROOT, Rso, static_exports, u32

MODULES = (
    ('hbm', 0x808196E0, '120d69080b793e636045b29a741c52c758c5c8de1700a638682f563644f714a2'),
    ('com', 0x80868E40, 'a6ec45859c0530d27d73f0cfecac23b96c8d1842ee0c562a515d50a3085c4deb'),
    ('lobby', 0x808C76A0, '4990c77dbf102b5ddcd784b0cd0ee8592413f9b12a779407240ba125fb206c46'),
    ('demo', 0x8097D080, '603076899fbb08678447f29089da2c99c0572d195a14d253463a20c847645beb'),
    ('quest', 0x809D3DC0, 'aed47cf7fb43c1ba38a0e0e15cdf0e18d1e775eaf407de9ba02d4a1c2947b359'),
)


def discover(module, BASE, rso, linked, addresses):
    text_offset, text_size = rso.sections[1]
    seeds = {}
    for name, offset, section in rso.exports:
        if section == 1:
            if offset % 4 or offset >= text_size:
                raise ValueError('Invalid RSO text export')
            seeds[BASE + text_offset + offset] = module + '_' + name
    for h, o, label in ((0x20, 0x24, 'prolog'), (0x21, 0x28, 'epilog'), (0x22, 0x2C, 'unresolved')):
        if rso.data[h] != 1:
            raise ValueError('RSO entry is not in text section')
        seeds[addresses[1] + u32(rso.data, o)] = module + '_' + label
    # Constructor/destructor tail stubs may have neither an export nor a stack
    # prologue. These section identities are verified for the five pinned boot modules.
    for section, label in ((2, 'ctor'), (3, 'dtor')):
        offset, size = rso.sections[section]
        terminated = False
        if not offset or size % 4:
            raise ValueError('Invalid RSO constructor/destructor section')
        for pos in range(offset, offset + size, 4):
            address = u32(linked, pos)
            if not address:
                terminated = True
                continue
            if terminated or address % 4 or not addresses[1] <= address < addresses[1] + text_size:
                raise ValueError('Invalid RSO constructor/destructor pointer')
            seeds.setdefault(address, f'{module}_{label}_{address:08X}')
        if not terminated:
            raise ValueError('Unterminated RSO constructor/destructor section')
    for pos in range(text_offset, text_offset + text_size - 4, 4):
        word, following = struct.unpack_from('>II', linked, pos)
        if word & 0xFFFF8000 == 0x94218000 and following == 0x7C0802A6:
            seeds.setdefault(BASE + pos, f'{module}_func_{BASE + pos:08X}')
    return seeds


def main():
    dol = bytearray((ROOT / 'Assets/DATA/sys/main.dol').read_bytes())
    imports = static_exports()
    images = []
    seeds = {}
    bindings = ['// Generated verified fixed-address boot bindings.\n#pragma once\n#include <cstdint>\n']
    for module, base, digest in MODULES:
        rso = Rso((ROOT / f'Assets/DATA/files/01/{module}_data.rso').read_bytes())
        if hashlib.sha256(rso.data).hexdigest() != digest:
            raise ValueError(f'Unverified RMHP08 {module} module')
        linked, addresses = rso.link(base, imports)
        text_offset, text_size = rso.sections[1]
        images.append((base, linked, text_offset, text_size))
        module_seeds = discover(module, base, rso, linked, addresses)
        if seeds.keys() & module_seeds.keys():
            raise ValueError('Overlapping RSO function seeds')
        seeds.update(module_seeds)
        checksum = 0xCBF29CE484222325
        for byte in linked[text_offset:text_offset + text_size]:
            checksum = ((checksum ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        bindings.append(f'namespace {module.title()}Binding {{\n'
                        f'constexpr uint32_t Base = 0x{base:08X}, Text = 0x{addresses[1]:08X}, Size = 0x{text_size:X};\n'
                        f'constexpr uint64_t Checksum = 0x{checksum:016X}ULL;\n}}\n')
        print(f'{module}: {len(module_seeds)} seeds, text {addresses[1]:08X}+{text_size:X}')
    # The analysis DOL has five free text slots but only two free data slots.
    # A single data span contains all linked images, including identical text
    # bytes also described by the executable slots. Gaps are analysis-only zeros.
    # ProgramImageBuilder copies identical bytes over the text ranges; runtime
    # initialization continues to use the original DOL exclusively.
    first = images[0][0]
    overlay = bytearray(images[-1][0] + len(images[-1][1]) - first)
    previous_end = first
    for base, linked, _, _ in images:
        if base < previous_end:
            raise ValueError('Overlapping RSO images')
        overlay[base-first:base-first+len(linked)] = linked
        previous_end = base + len(linked)
    file_offset = len(dol)
    dol.extend(overlay)
    def section(slot, address, size):
        if u32(dol, 0x90 + slot * 4):
            raise ValueError('Analysis DOL slot already occupied')
        for table, value in ((0, file_offset + address - first), (0x48, address), (0x90, size)):
            struct.pack_into('>I', dol, table + slot * 4, value)
    for slot, (base, _, offset, size) in enumerate(images, 2):
        section(slot, base + offset, size)
    section(15, first, len(overlay))
    (ROOT / 'build/translation.dol').write_bytes(dol)
    config = (ROOT / 'recomp.yml').read_text().replace('workspace_root: .', 'workspace_root: ..')
    config = config.replace('Assets/DATA/sys/main.dol', 'build/translation.dol')
    config = config.replace(hashlib.sha256((ROOT / 'Assets/DATA/sys/main.dol').read_bytes()).hexdigest(),
                            hashlib.sha256(dol).hexdigest())
    (ROOT / 'build/recomp-analysis.yml').write_text(config)
    with (ROOT / 'build/MAP.txt').open('a') as out:
        for address, name in sorted(seeds.items()):
            out.write(f'{address:08x} {name}\n')
    binding = ROOT / 'generated/rso_binding.h'
    if not binding.exists() or binding.read_text() != ''.join(bindings):  # keep its mtime for the build
        binding.write_text(''.join(bindings))


if __name__ == '__main__':
    main()
