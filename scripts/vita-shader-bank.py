#!/usr/bin/env python3
"""Compile and embed the GXM shader bank (docs/optimizations/rendering/offline-shader-bank.md).

Reads the stage sources that mh3-gxm-shader-bank wrote (cg/KEY_STAGE.cg and
manifest.tsv), compiles every stage offline with Sony's psp2cgc in the
psp2cgc-wine Docker image, and writes:

  shader_bank.bin         zstd dictionary, then each program as a zstd frame
  shader_bank_tables.cpp  sorted (key, offset, compressed size, size) tables
  shader_bank.S           .incbin of shader_bank.bin
  shader_bank.json        counts, sizes, compiler identity, -O2 stages

Compiled programs are cached by SHA-256 of compiler, options, profile and
source, so only changed stages are compiled. The GXM runtime decompresses a
program at its first use (aurora-main/lib/gxm/shader_bank.cpp).
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

OPTIONS = ['-O3', '-fastmath', '-nofastprecision', '-fastint']
# psp2cgc 3.50's -O3 optimizer crashes on a few stages (an internal compiler
# error or a wild write); those compile at -O2. The runtime compiler matched
# OPTIONS (vitaShaRK SHARK_OPT_FAST, fast math and int, no fast precision).
FALLBACK_OPTIONS = ['-O2', '-fastmath', '-nofastprecision', '-fastint']
PROFILES = {'v': 'sce_vp_psp2', 'f': 'sce_fp_psp2', 'r': 'sce_fp_psp2'}
STAGES = [('v', 'Vertex'), ('f', 'Fragment'), ('r', 'FragmentScalar')]
# A 256 KiB dictionary shrinks the 13.5 MB of GXP to under 1 MB.
DICTIONARY_BYTES = 256 * 1024
LEVEL = 19


def manifest(sources):
    entries = []
    for line in (sources / 'manifest.tsv').read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        key, stage, kind, _, name = line.split('\t', 4)
        if stage not in PROFILES or len(key) != 16:
            raise ValueError(f'Bad manifest line: {line}')
        entries.append({'key': key, 'stage': stage, 'kind': kind, 'name': name,
                        'source': sources / 'cg' / f'{key}_{stage}.cg'})
    if not entries:
        raise ValueError('Empty shader bank manifest')
    return entries


def compiler_identity(psp2cgc):
    return hashlib.sha256(psp2cgc.read_bytes()).hexdigest()


def cache_name(identity, stage, source):
    digest = hashlib.sha256()
    for part in (identity, ' '.join(OPTIONS), PROFILES[stage]):
        digest.update(part.encode() + b'\0')
    digest.update(source)
    return digest.hexdigest()


def valid_gxp(data):
    # SceGxmProgram: "GXP\0", version, SDK version, declared size at 8.
    return len(data) >= 64 and data[:4] == b'GXP\0' and 64 <= struct.unpack_from('<I', data, 8)[0] <= len(data)


def ensure_image(image):
    """Build the psp2cgc-wine image from docker/ when it is missing."""
    if subprocess.run(['docker', 'image', 'inspect', image], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
        return
    docker = Path(__file__).resolve().parent.parent / 'docker'
    subprocess.run(['docker', 'build', '-t', image, '-f', str(docker / 'psp2cgc-wine.Dockerfile'), str(docker)], check=True)


def run_compiler(work, options, psp2cgc, image, jobs):
    """Compile every work/*.cg lacking a .gxp; a failure leaves none."""
    # QEMU_CPU: on arm64 hosts the i386 compiler runs under qemu-user, whose
    # default CPU model makes psp2cgc crash on more stages; core2duo output is
    # byte-identical where both succeed. Ignored on x86 hosts.
    script = r'''
set -u
wineboot -u >/dev/null 2>&1 || exit 1
wineserver -w || exit 1
one() {
  source=$1
  [ -f "${source%.cg}.gxp" ] && return 0
  case "$source" in *_v.cg) profile=sce_vp_psp2;; *) profile=sce_fp_psp2;; esac
  if wine /psp2cgc/psp2cgc.exe $OPTIONS -profile "$profile" "$source" -o "${source%.cg}.gxp.tmp" > "${source%.cg}.log" 2>&1; then
    mv "${source%.cg}.gxp.tmp" "${source%.cg}.gxp"
  else
    rm -f "${source%.cg}.gxp.tmp"
  fi
}
export -f one
printf '%s\0' /work/*.cg | xargs -0 -n 1 -P "$JOBS" bash -c 'one "$1"' _
chown -R "$OWNER" /work
'''
    command = ['docker', 'run', '--rm', '--pull', 'never', '-e', 'WINEDEBUG=-all', '-e', 'QEMU_CPU=core2duo',
               '-e', f'JOBS={jobs}', '-e', f'OWNER={os.getuid()}:{os.getgid()}', '-e', 'OPTIONS=' + ' '.join(options),
               '-v', f'{psp2cgc}:/psp2cgc/psp2cgc.exe:ro', '-v', f'{work}:/work', image, 'bash', '-c', script]
    subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)


def compile_missing(missing, cache, psp2cgc, image, jobs):
    """Compile [(cache name, entry)]; programs and their options land in cache."""
    work = Path(tempfile.mkdtemp(prefix='gxm-bank-', dir=cache))
    try:
        for name, entry in missing:
            shutil.copyfile(entry['source'], work / f'{name}_{entry["stage"]}.cg')
        print(f'vita-shader-bank: compiling {len(missing)} stages with psp2cgc', flush=True)
        pending = list(missing)
        for options in (OPTIONS, FALLBACK_OPTIONS):
            if options is FALLBACK_OPTIONS:
                print(f'vita-shader-bank: {len(pending)} stages failed at {OPTIONS[0]}; retrying at {options[0]}', flush=True)
            run_compiler(work, options, psp2cgc, image, jobs)
            remaining = []
            for name, entry in pending:
                output = work / f'{name}_{entry["stage"]}.gxp'
                if output.exists() and valid_gxp(output.read_bytes()):
                    os.replace(output, cache / f'{name}.gxp')
                    write_if_changed(cache / f'{name}.options', ' '.join(options) + '\n')
                else:
                    output.unlink(missing_ok=True)
                    remaining.append((name, entry))
            pending = remaining
            if not pending:
                break
        if pending:
            failed = []
            for name, entry in pending:
                log = work / f'{name}_{entry["stage"]}.log'
                failed.append(f'{entry["key"]}_{entry["stage"]} ({entry["name"]}): ' + (log.read_text().strip() if log.exists() else 'no output'))
            raise RuntimeError('psp2cgc failed for:\n  ' + '\n  '.join(failed))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def seed(cache, identity, directory):
    """Adopt NAME_S.cg / NAME_S.gxp pairs compiled earlier with OPTIONS."""
    adopted = 0
    for source in sorted(directory.glob('*.cg')):
        stage = source.stem.rsplit('_', 1)[-1]
        program = source.with_suffix('.gxp')
        if stage not in PROFILES or not program.exists() or not valid_gxp(program.read_bytes()):
            continue
        name = cache_name(identity, stage, source.read_bytes())
        if not (cache / f'{name}.gxp').exists():
            shutil.copyfile(program, cache / f'{name}.gxp')
            write_if_changed(cache / f'{name}.options', ' '.join(OPTIONS) + '\n')
            adopted += 1
    print(f'vita-shader-bank: adopted {adopted} programs from {directory}')


def write_if_changed(path, data):
    data = data.encode() if isinstance(data, str) else data
    if path.exists() and path.read_bytes() == data:
        return
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_bytes(data)
    os.replace(temporary, path)


def embed(entries, programs, output, identity, fallback):
    """Compress each program with a shared dictionary; emit the tables."""
    with tempfile.TemporaryDirectory(prefix='gxm-bank-dict-') as scratch:
        scratch = Path(scratch)
        samples = []
        for index, program in enumerate(programs):
            sample = scratch / f'{index}.gxp'
            sample.write_bytes(program)
            samples.append(str(sample))
        dictionary = scratch / 'dictionary'
        # Deterministic: single-threaded training with fixed parameters.
        subprocess.run(['zstd', '-q', '--train', '-T1', f'--maxdict={DICTIONARY_BYTES}', '-o', str(dictionary), *samples],
                       check=True, stdout=subprocess.DEVNULL)
        dictionary_bytes = dictionary.read_bytes()
        compressed = scratch / 'compressed'
        compressed.mkdir()
        subprocess.run(['zstd', '-q', f'-{LEVEL}', '-D', str(dictionary), '--no-check', '--output-dir-flat', str(compressed), *samples],
                       check=True)
        frames = [(compressed / f'{index}.gxp.zst').read_bytes() for index in range(len(programs))]
        # Every frame must decode to its exact program with the embedded dictionary.
        decoded = scratch / 'decoded'
        decoded.mkdir()
        subprocess.run(['zstd', '-q', '-d', '-D', str(dictionary), '--output-dir-flat', str(decoded),
                        *(str(compressed / f'{index}.gxp.zst') for index in range(len(programs)))], check=True)
        for index, program in enumerate(programs):
            if (decoded / f'{index}.gxp').read_bytes() != program:
                raise RuntimeError(f'zstd round trip changed {entries[index]["key"]}_{entries[index]["stage"]}')
    blob = bytearray(dictionary_bytes)
    tables = {stage: [] for stage, _ in STAGES}
    for entry, program, frame in zip(entries, programs, frames):
        tables[entry['stage']].append((int(entry['key'], 16), len(blob), len(frame), len(program)))
        blob += frame
    text = ['// Generated by scripts/vita-shader-bank.py; do not edit.',
            '#include "shader_bank.hpp"',
            'extern "C" const unsigned char aurora_gxm_shader_bank[];',
            'namespace aurora::gxm::shader_bank {',
            'const unsigned char* const Blob = aurora_gxm_shader_bank;',
            f'const uint32_t DictionaryBytes = {len(dictionary_bytes)};']
    summary = {'compiler_sha256': identity, 'options': ' '.join(OPTIONS), 'fallback_options': ' '.join(FALLBACK_OPTIONS),
               'fallback_stages': fallback, 'dictionary_bytes': len(dictionary_bytes), 'blob_bytes': len(blob), 'stages': {}}
    for stage, name in STAGES:
        rows = sorted(tables[stage])
        if any(a[0] == b[0] for a, b in zip(rows, rows[1:])):
            raise RuntimeError(f'Duplicate {name} key in the shader bank')
        text.append(f'const Program {name}Programs[] = {{')
        text += [f'    {{0x{key:016x}ull, {offset}u, {packed}u, {size}u}},' for key, offset, packed, size in rows]
        if not rows:
            text.append('    {0, 0, 0, 0},')
        text.append('};')
        text.append(f'const size_t {name}ProgramCount = {len(rows)};')
        summary['stages'][stage] = {'programs': len(rows), 'bytes': sum(r[3] for r in rows), 'compressed_bytes': sum(r[2] for r in rows)}
    text.append('} // namespace aurora::gxm::shader_bank')
    write_if_changed(output / 'shader_bank.bin', bytes(blob))
    write_if_changed(output / 'shader_bank_tables.cpp', '\n'.join(text) + '\n')
    write_if_changed(output / 'shader_bank.S', '// Generated by scripts/vita-shader-bank.py.\n'
                     '.section .rodata.aurora_gxm_shader_bank,"a",%progbits\n.balign 64\n'
                     '.global aurora_gxm_shader_bank\naurora_gxm_shader_bank:\n'
                     f'.incbin "{(output / "shader_bank.bin").resolve()}"\n'
                     '.section .note.GNU-stack,"",%progbits\n')
    write_if_changed(output / 'shader_bank.json', json.dumps(summary, indent=2) + '\n')
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('sources', type=Path, help='mh3-gxm-shader-bank output (cg/, manifest.tsv)')
    parser.add_argument('output', type=Path, help='directory for the embedded bank files')
    parser.add_argument('--cache', type=Path, default=Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'mh3-gxm-shader-bank')
    parser.add_argument('--psp2cgc', type=Path, default=os.environ.get('PSP2CGC'), required='PSP2CGC' not in os.environ,
                        help="absolute path to Sony's psp2cgc.exe (default: $PSP2CGC)")
    parser.add_argument('--image', default='psp2cgc-wine')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    parser.add_argument('--seed', type=Path, help='adopt NAME_S.cg/.gxp pairs compiled earlier with the bank options')
    args = parser.parse_args()
    entries = manifest(args.sources)
    args.cache.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    identity = compiler_identity(args.psp2cgc)
    if args.seed:
        seed(args.cache, identity, args.seed)
    names = [cache_name(identity, entry['stage'], entry['source'].read_bytes()) for entry in entries]
    missing = [(name, entry) for name, entry in zip(names, entries) if not (args.cache / f'{name}.gxp').exists()]
    if missing:
        ensure_image(args.image)
        compile_missing(missing, args.cache, args.psp2cgc.resolve(), args.image, args.jobs)
    programs, fallback = [], []
    for name, entry in zip(names, entries):
        data = (args.cache / f'{name}.gxp').read_bytes()
        if not valid_gxp(data):
            raise RuntimeError(f'Invalid cached GXP for {entry["key"]}_{entry["stage"]}; delete {args.cache / (name + ".gxp")}')
        programs.append(data)
        options = args.cache / f'{name}.options'
        if options.exists() and options.read_text().split() != OPTIONS:
            fallback.append(f'{entry["key"]}_{entry["stage"]} {entry["name"]}')
    summary = embed(entries, programs, args.output, identity, fallback)
    counts = ', '.join(f'{s["programs"]} {stage}' for stage, s in summary['stages'].items())
    raw = sum(s['bytes'] for s in summary['stages'].values())
    print(f'vita-shader-bank: {counts}; {raw} bytes of GXP, {summary["blob_bytes"]} embedded; '
          f'{len(missing)} compiled, {len(entries) - len(missing)} cached, {len(fallback)} at {FALLBACK_OPTIONS[0]}')


if __name__ == '__main__':
    main()
