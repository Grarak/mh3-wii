#!/usr/bin/env python3
"""Compile verified RSO overlays separately from address-overlapping boot code."""
import hashlib
import json
import os
import re
import struct
import subprocess
from pathlib import Path
from link_rso import ROOT, Rso, static_exports, u32
from prepare_rso_translation import discover

NAME = 'map_village'
BASE = 0x8097D080
DIGEST = 'aeddd064cda874ff0c4ed0067aa27ef7f165e4190ae0e8328e1326156d2b96dc'
DOL_DIGEST = 'db5c9939f677c33d15d446aa4ad19c28044d0b80c5b3cc75adc7f1340ecab162'


def isolate_source(source, address):
    symbol = f'func_{address:08X}'
    # These six bootstrap functions use CPU-context calls or fully inlined
    # helpers. Reject a future emission requiring another profile's typed ABI.
    symbols = set(re.findall(r'\bfunc_[A-Fa-f0-9]{8}\w*', source))
    if symbols != {symbol} or 'InvokeDirectCpu<' in source:
        raise ValueError('Overlay needs explicit cross-profile call handling')
    return re.sub(r'\b' + symbol + r'\b', f'mh3_{NAME}_{address:08X}', source)


def write_changed(path, text):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def main():
    work = ROOT / 'build/rso-overlays' / NAME
    work.mkdir(parents=True, exist_ok=True)
    rso = Rso((ROOT / f'Assets/DATA/files/01/{NAME}.rso').read_bytes())
    if hashlib.sha256(rso.data).hexdigest() != DIGEST:
        raise ValueError('Unverified map_village module')
    image, addresses = rso.link(BASE, static_exports())
    offset, size = rso.sections[1]
    text = addresses[1]
    prolog = addresses[rso.data[0x20]] + u32(rso.data, 0x24)
    seeds = discover(NAME, BASE, rso, image, addresses)
    dol = bytearray((ROOT / 'Assets/DATA/sys/main.dol').read_bytes())
    if hashlib.sha256(dol).hexdigest() != DOL_DIGEST:
        raise ValueError('Unverified DOL')
    file_offset = len(dol)
    dol.extend(image)
    for slot, address, length, position in ((2, text, size, file_offset + offset),
                                          (15, BASE, len(image), file_offset)):
        if u32(dol, 0x90 + slot * 4):
            raise ValueError('Overlay analysis DOL slot occupied')
        for table, value in ((0, position), (0x48, address), (0x90, length)):
            struct.pack_into('>I', dol, table + slot * 4, value)
    (work / 'analysis.dol').write_bytes(dol)
    # Base DOL seeds only: the boot RSOs have overlapping and unrelated code.
    lines = [line for line in (ROOT / 'build/MAP.txt').read_text().splitlines()
             if int(line.split()[0], 16) < 0x80800000]
    lines += [f'{address:08x} {name}' for address, name in sorted(seeds.items())]
    write_changed(work / 'MAP.txt', '\n'.join(lines) + '\n')
    config = (ROOT / 'recomp.yml').read_text().replace('workspace_root: .', 'workspace_root: ../../..')
    config = config.replace('Assets/DATA/sys/main.dol', f'build/rso-overlays/{NAME}/analysis.dol')
    config = config.replace('build/MAP.txt', f'build/rso-overlays/{NAME}/MAP.txt')
    config = config.replace(DOL_DIGEST, hashlib.sha256(dol).hexdigest())
    config = config.replace('0x80006310', f'0x{prolog:08X}')
    write_changed(work / 'recomp.yml', config)
    toolchain = Path(os.environ.get('WIICOMPILED_ROOT', str(ROOT / 'toolchain')))
    translator = toolchain / 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
    subprocess.run([os.environ.get('DOTNET_BIN', str(ROOT / 'tools/dotnet/dotnet')), str(translator),
                    'translate-recursive', f'0x{prolog:08X}', '--project', str(work / 'recomp.yml'),
                    '--outdir', str(work / 'functions'), '--output-metadata', str(work / 'translation.json'),
                    '--threads', '8', '--prune-stale'], cwd=ROOT, check=True)
    metadata = json.loads((work / 'translation.json').read_text())
    if any(metadata['quality'][key] for key in ('unsupportedInstructionCount', 'invalidSsaFunctionCount')):
        raise ValueError('Overlay translation quality check failed')
    functions = [entry for entry in metadata['functions'] if text <= entry['entryPoint'] < text + size]
    if {entry['entryPoint'] for entry in functions} != set(seeds):
        raise ValueError('Unexpected overlay function discovery')
    sources, records = [], []
    for entry in sorted(functions, key=lambda item: item['entryPoint']):
        source = (work / 'functions' / entry['relativePath']).read_text()
        if hashlib.sha256(source.encode()).hexdigest() != entry['sha256']:
            raise ValueError('Generated overlay source hash mismatch')
        if entry['build']['directCallDependencies']:
            raise ValueError('Overlay needs explicit cross-profile ABI dependencies')
        address = entry['entryPoint']
        sources.append(isolate_source(source, address))
        mask = entry['build']['nonvolatileFprWriteMask']
        records.append(f'    {{0x{address:08X}u, &mh3_{NAME}_{address:08X}, 0x{mask:08X}u, false}},')
    checksum = 0xCBF29CE484222325
    for byte in image[offset:offset + size]:
        checksum = ((checksum ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    header = f'''// Generated static overlay; no runtime instruction translation.
#pragma once
#include <cstdint>
struct RawDispatchRecord;
namespace MapVillageBinding {{
constexpr uint32_t Base = 0x{BASE:08X}, Text = 0x{text:08X}, Size = 0x{size:X}, Prolog = 0x{prolog:08X};
constexpr uint64_t Checksum = 0x{checksum:016X}ULL;
constexpr unsigned Count = {len(records)};
extern const RawDispatchRecord Records[Count];
}}
'''
    output = ROOT / 'generated/rso_overlays'
    output.mkdir(parents=True, exist_ok=True)
    write_changed(output / 'map_village.h', header)
    sources += ['namespace MapVillageBinding {\nconst RawDispatchRecord Records[]{\n' +
                '\n'.join(records) + '\n};\n}\n']
    # Include the declaration first so the const table has external linkage.
    write_changed(output / 'map_village.cpp', '#include "map_village.h"\n' + '\n'.join(sources))
    print(f'Emitted {len(records)} isolated {NAME} functions, linked text {text:08X}+{size:X}')


if __name__ == '__main__':
    main()
