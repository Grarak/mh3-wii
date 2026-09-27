#!/usr/bin/env python3
"""Add the verified first RSO to an analysis-only DOL; preserve boot data init.

This is deliberately a fixed-address first-module implementation. Further RSO
loads need their own bindings, relocation validation and unload handling.
"""
import hashlib
import struct
from pathlib import Path
from link_rso import ROOT, Rso, static_exports, u32

BASE = 0x808196E0


def main():
    path = ROOT / 'Assets/DATA/files/01/hbm_data.rso'
    rso = Rso(path.read_bytes())
    if hashlib.sha256(rso.data).hexdigest() != '120d69080b793e636045b29a741c52c758c5c8de1700a638682f563644f714a2':
        raise ValueError('First-module binding requires the verified RMHP08 hbm_data.rso')
    linked, addresses = rso.link(BASE, static_exports())
    dol = bytearray((ROOT / 'Assets/DATA/sys/main.dol').read_bytes())
    text_offset, text_size = rso.sections[1]
    # Three unused DOL slots describe the RSO header, text and remaining data.
    # This file is solely translator input, never the runtime initializer.
    for slot, offset, size in ((2, text_offset, text_size), (15, 0, text_offset),
                              (16, text_offset + text_size, len(linked) - text_offset - text_size)):
        if u32(dol, 0x90 + slot * 4):
            raise ValueError('Analysis DOL slot already occupied')
        for table, value in ((0, len(dol)), (0x48, BASE + offset), (0x90, size)):
            struct.pack_into('>I', dol, table + slot * 4, value)
        dol.extend(linked[offset:offset + size])
    (ROOT / 'build/translation.dol').write_bytes(dol)
    config = (ROOT / 'recomp.yml').read_text().replace('workspace_root: .', 'workspace_root: ..')
    config = config.replace('Assets/DATA/sys/main.dol', 'build/translation.dol')
    config = config.replace(hashlib.sha256((ROOT / 'Assets/DATA/sys/main.dol').read_bytes()).hexdigest(),
                            hashlib.sha256(dol).hexdigest())
    (ROOT / 'build/recomp-analysis.yml').write_text(config)
    seeds = {}
    for name, offset, section in rso.exports:
        if section == 1:
            if offset % 4 or offset >= text_size:
                raise ValueError('Invalid RSO text export')
            seeds[BASE + text_offset + offset] = 'hbm_' + name
    for h, o, label in ((0x20, 0x24, 'prolog'), (0x21, 0x28, 'epilog'), (0x22, 0x2C, 'unresolved')):
        if rso.data[h] != 1:
            raise ValueError('RSO entry is not in text section')
        seeds[addresses[1] + u32(rso.data, o)] = 'hbm_' + label
    # Constructor/destructor tail stubs may have neither an export nor a stack
    # prologue. These section identities are verified for hbm_data.rso.
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
            seeds.setdefault(address, f'hbm_{label}_{address:08X}')
        if not terminated:
            raise ValueError('Unterminated RSO constructor/destructor section')
    for pos in range(text_offset, text_offset + text_size - 4, 4):
        word, following = struct.unpack_from('>II', linked, pos)
        if word & 0xFFFF8000 == 0x94218000 and following == 0x7C0802A6:
            seeds.setdefault(BASE + pos, f'hbm_func_{BASE + pos:08X}')
    with (ROOT / 'build/MAP.txt').open('a') as out:
        for address, name in sorted(seeds.items()):
            out.write(f'{address:08x} {name}\n')
    checksum = 0xCBF29CE484222325
    for byte in linked[text_offset:text_offset + text_size]:
        checksum = ((checksum ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    (ROOT / 'generated/rso_binding.h').write_text(
        '// Generated from locally supplied RSO; fixed first-module binding.\n#pragma once\n'
        '#include <cstdint>\nnamespace HbmBinding {\n'
        f'constexpr uint32_t Base = 0x{BASE:08X}, Text = 0x{addresses[1]:08X}, Size = 0x{text_size:X};\n'
        f'constexpr uint64_t Checksum = 0x{checksum:016X}ULL;\n}}\n')
    print(f'Analysis-only RSO overlay: {len(seeds)} seeds, text {addresses[1]:08X}+{text_size:X}')


if __name__ == '__main__':
    main()
