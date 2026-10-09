#!/usr/bin/env python3
"""Compile verified RSO overlays separately from address-overlapping boot code."""
import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
from pathlib import Path
from link_rso import ROOT, Rso, static_exports, u32
from prepare_rso_translation import discover

PROFILES = {
    'map_village': ('map_village', 0x8097D080,
                    'aeddd064cda874ff0c4ed0067aa27ef7f165e4190ae0e8328e1326156d2b96dc', 'MapVillageBinding'),
    'quest_field': ('quest_data', 0x808C76A0,
                    'aed47cf7fb43c1ba38a0e0e15cdf0e18d1e775eaf407de9ba02d4a1c2947b359', 'QuestFieldBinding'),
    'enemy_field': ('em_data', 0x80972F40,
                    'bbb8a3003bc52f38e4868d49164fc1a91aa884c06a3d491e81b87f473621c0a9', 'EnemyFieldBinding'),
    'map01_field': ('map01', 0x80AA4500,
                    'f93ed63095e860360533616806ffe10219c0f587d137f976e6b9429f83ead26d', 'Map01FieldBinding'),
}
DOL_DIGEST = 'db5c9939f677c33d15d446aa4ad19c28044d0b80c5b3cc75adc7f1340ecab162'


def write_changed(path, text):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def isolate_profile_source(source, profile):
    source = re.sub(r'\bfunc_([A-Fa-f0-9]{8}\w*)', rf'mh3_{profile}_\1', source)
    source = re.sub(r'InvokeDirectCpu<(0x[0-9A-Fa-f]+u)>\(ctx\)', r'InvokeIndirectCpu(\1, ctx)', source)
    if 'InvokeDirectCpu<' in source:
        raise ValueError('Unrecognized direct-call form in isolated overlay')
    return source


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--profile', choices=PROFILES, default='map_village')
    profile = parser.parse_args().profile
    name, base, digest, binding = PROFILES[profile]
    work = ROOT / 'build/rso-overlays' / profile
    work.mkdir(parents=True, exist_ok=True)
    rso = Rso((ROOT / f'Assets/DATA/files/01/{name}.rso').read_bytes())
    if hashlib.sha256(rso.data).hexdigest() != digest:
        raise ValueError(f'Unverified {name} module')
    image, addresses = rso.link(base, static_exports())
    offset, size = rso.sections[1]
    text = addresses[1]
    prolog = addresses[rso.data[0x20]] + u32(rso.data, 0x24)
    seeds = discover(name, base, rso, image, addresses)
    if profile == 'quest_field':
        # Preserve verified tail-call boundaries from the boot-address profile.
        for line in (ROOT / 'symbols.txt').read_text().splitlines():
            fields = line.split('#', 1)[0].split()
            if len(fields) == 2 and 0x809D3EB0 <= int(fields[0], 16) < 0x80A07918:
                seeds[int(fields[0], 16) - 0x809D3DC0 + base] = fields[1]
    dol = bytearray((ROOT / 'Assets/DATA/sys/main.dol').read_bytes())
    if hashlib.sha256(dol).hexdigest() != DOL_DIGEST:
        raise ValueError('Unverified DOL')
    file_offset = len(dol)
    dol.extend(image)
    for slot, address, length, position in ((2, text, size, file_offset + offset),
                                          (15, base, len(image), file_offset)):
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
    config = config.replace('Assets/DATA/sys/main.dol', f'build/rso-overlays/{profile}/analysis.dol')
    config = config.replace('build/MAP.txt', f'build/rso-overlays/{profile}/MAP.txt')
    config = config.replace(DOL_DIGEST, hashlib.sha256(dol).hexdigest())
    config = config.replace('0x80006310', f'0x{prolog:08X}')
    write_changed(work / 'recomp.yml', config)
    toolchain = Path(os.environ.get('WIICOMPILED_ROOT', str(ROOT / 'toolchain')))
    translator = toolchain / 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
    subprocess.run([os.environ.get('DOTNET_BIN', str(ROOT / 'tools/dotnet/dotnet')), str(translator),
                    'translate-recursive', f'0x{prolog:08X}', '--project', str(work / 'recomp.yml'),
                    '--outdir', str(work / 'functions'), '--output-metadata', str(work / 'translation.json'),
                    '--threads', '8', '--max-instructions', '32768',
                    '--max-bytes', '262144', '--prune-stale'], cwd=ROOT, check=True)
    metadata = json.loads((work / 'translation.json').read_text())
    if any(metadata['quality'][key] for key in ('unsupportedInstructionCount', 'invalidSsaFunctionCount')):
        raise ValueError('Overlay translation quality check failed')
    functions = [entry for entry in metadata['functions'] if text <= entry['entryPoint'] < text + size]
    if not set(seeds).issubset({entry['entryPoint'] for entry in functions}):
        raise ValueError('Unexpected overlay function discovery')
    # Include same-translation typed callees rather than assuming their ABI
    # matches the independently translated boot image. CPU-context calls use
    # runtime dispatch, which selects the active overlay at overlapping addresses.
    entries = {entry['entryPoint']: entry for entry in metadata['functions']}
    selected = {entry['entryPoint'] for entry in functions}
    pending = list(selected)
    while pending:
        address = pending.pop()
        source = (work / 'functions' / entries[address]['relativePath']).read_text()
        for symbol in re.findall(r'\bfunc_([A-Fa-f0-9]{8})\w*', source):
            dependency = int(symbol, 16)
            if dependency not in selected:
                if dependency not in entries:
                    raise ValueError(f'Missing typed dependency {dependency:08X}')
                selected.add(dependency)
                pending.append(dependency)
    sources, records = [], []
    for entry in sorted((entries[a] for a in selected), key=lambda item: item['entryPoint']):
        source = (work / 'functions' / entry['relativePath']).read_text()
        if hashlib.sha256(source.encode()).hexdigest() != entry['sha256']:
            raise ValueError('Generated overlay source hash mismatch')
        address = entry['entryPoint']
        sources.append(isolate_profile_source(source, profile))
        mask = entry['build']['nonvolatileFprWriteMask']
        if text <= address < text + size:
            records.append(f'    {{0x{address:08X}u, &mh3_{profile}_{address:08X}, 0x{mask:08X}u, false}},')
    checksum = 0xCBF29CE484222325
    for byte in image[offset:offset + size]:
        checksum = ((checksum ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    header = f'''// Generated static overlay; no runtime instruction translation.
#pragma once
#include <cstdint>
struct RawDispatchRecord;
namespace {binding} {{
constexpr uint32_t Base = 0x{base:08X}, Text = 0x{text:08X}, Size = 0x{size:X}, Prolog = 0x{prolog:08X};
constexpr uint64_t Checksum = 0x{checksum:016X}ULL;
constexpr unsigned Count = {len(records)};
extern const RawDispatchRecord Records[Count];
}}
'''
    output = ROOT / 'generated/rso_overlays'
    output.mkdir(parents=True, exist_ok=True)
    write_changed(output / f'{profile}.h', header)
    sources += [f'namespace {binding} {{\nconst RawDispatchRecord Records[]{{\n' +
                '\n'.join(records) + '\n};\n}\n']
    # Include the declaration first so the const table has external linkage.
    write_changed(output / f'{profile}.cpp', f'#include "{profile}.h"\n' + '\n'.join(sources))
    print(f'Emitted {len(records)} isolated {name} functions, linked text {text:08X}+{size:X}')


if __name__ == '__main__':
    main()
