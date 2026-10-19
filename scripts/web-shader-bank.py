#!/usr/bin/env python3
"""Convert and embed the WebGPU shader bank.

Reads the GLSL ES stages that mh3-web-shader-bank wrote (glsl/KEY_STAGE.glsl
and manifest.tsv) and converts each to WGSL: the declarations are rewritten to
Vulkan GLSL with fixed bindings (below), glslangValidator compiles it to
SPIR-V and naga writes WGSL. Writes:

  shader_bank.bin         zstd dictionary, then each stage as a zstd frame
  shader_bank_tables.cpp  sorted (key, offset, compressed size, size) tables
                          and the #embed of shader_bank.bin
  shader_bank.json        counts, sizes, tool versions

Each WGSL stage starts with comment lines the backend reads
(aurora-main/lib/webgpu/shader_bank.cpp):

  // mh3-blocks NAME=BINDING ...      uniform blocks, group 0
  // mh3-loose NAME ...               loose vec4 uniforms, in declaration order
  // mh3-textures NAME:KIND:USE ...   group 1 slot k (texture 2k, sampler 2k+1),
                                      KIND f/i/u, USE sample/fetch

Bindings: group 0 holds the uniform blocks at their GL binding points (Draw 0,
Scene 1, Palette 2, Shade 3, Material 4, Animated 5, View 6, UVTransform 0)
and the loose uniforms of the vertex (6) and fragment (7) stage, which no
stage with a View block has; group 1 holds the
sampled textures in declaration order. Varyings get one location per name
across all programs. Vertex outputs keep WebGPU's conventions: the GLSL ES
stage writes GL clip space (y negated, z in [-w, w]), which a wrapper undoes.

Converted stages are cached by SHA-256 of the tool versions, the converter and
the source, so only changed stages are converted.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path

BLOCKS = {'Draw': 0, 'Scene': 1, 'Palette': 2, 'Shade': 3, 'Material': 4, 'Animated': 5, 'View': 6, 'UVTransform': 0}
LOOSE_BINDING = {'v': 6, 'f': 7}

# Fed by raw U8 matrix selector bytes (uint vertex formats on WebGPU).
UINT_INPUTS = {'a_select', 'a_select0', 'a_select1', 'a_select2', 'a_bones'}

# One location per varying name, shared by every vertex and fragment stage.
VARYINGS = ['v_color0', 'v_uv0', 'v_uv1', 'v_uv2', 'v_fog', 'v_uv', 'v_atlas_uv', 'v_sample0', 'v_sample1', 'v_sample2', 'v_sample3', 'v_base', 'v_slope']

# The GL -> WebGPU clip space undo, after the stage's own main().
POSITION = '''
void main() {
    aurora_main();
    gl_Position = vec4(gl_Position.x, -gl_Position.y, 0.5 * (gl_Position.z + gl_Position.w), gl_Position.w);
}
'''

STAGES = [('v', 'Vertex'), ('f', 'Fragment')]
DICTIONARY_BYTES = 512 * 1024
LEVEL = 19


def manifest(sources):
    entries = []
    for line in (sources / 'manifest.tsv').read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        key, stage, kind, name = line.split('\t', 3)
        if stage not in LOOSE_BINDING or len(key) != 16:
            raise ValueError(f'Bad manifest line: {line}')
        entries.append({'key': key, 'stage': stage, 'kind': kind, 'name': name,
                        'source': sources / 'glsl' / f'{key}_{stage}.glsl'})
    if not entries:
        raise ValueError('Empty shader bank manifest')
    return entries


def tool_identity(glslang, naga):
    versions = []
    for command in ([glslang, '--version'], [naga, '--version']):
        versions.append(subprocess.run(command, capture_output=True, text=True, check=True).stdout.strip())
    digest = hashlib.sha256(Path(__file__).read_bytes())
    for version in versions:
        digest.update(version.encode() + b'\0')
    return digest.hexdigest(), versions


def cache_name(identity, stage, source):
    digest = hashlib.sha256()
    digest.update(identity.encode() + b'\0' + stage.encode() + b'\0')
    digest.update(source)
    return digest.hexdigest()


def vulkan_glsl(preprocessed, stage):
    """Rewrites a preprocessed GLSL ES stage into Vulkan GLSL; returns it and its resources."""
    out, loose, textures, blocks = [], [], [], []
    loose_at = None
    for line in preprocessed.split('\n'):
        if line.startswith('#version'):
            out.append('#version 450')
            continue
        m = re.match(r'\s*layout\s*\(\s*std140\s*\)\s*uniform\s+(\w+)(.*)$', line)
        if m:
            if m.group(1) not in BLOCKS:
                raise ValueError(f'Uniform block {m.group(1)} has no binding')
            binding = BLOCKS[m.group(1)]
            blocks.append(f'{m.group(1)}={binding}')
            out.append(f'layout(std140, set=0, binding={binding}) uniform {m.group(1)}{m.group(2)}')
            continue
        m = re.match(r'\s*uniform\s+(?:highp\s+|mediump\s+|lowp\s+)?([iu]?)sampler2D\s+(\w+)\s*;\s*$', line)
        if m:
            kind, name = m.group(1), m.group(2)
            slot = len(textures)
            textures.append((name, kind or 'f'))
            out.append(f'layout(set=1, binding={2 * slot}) uniform {kind}texture2D {name}_t; '
                       f'layout(set=1, binding={2 * slot + 1}) uniform sampler {name}_s;')
            out.append(f'#define {name} {kind}sampler2D({name}_t, {name}_s)')
            continue
        m = re.match(r'\s*uniform\s+(?:highp\s+|mediump\s+|lowp\s+)?(\w+)\s+(\w+)\s*;\s*(//.*)?$', line)
        if m:
            if m.group(1) != 'vec4':
                raise ValueError(f'Loose uniform {m.group(2)} is a {m.group(1)}; only vec4 is supported')
            if loose_at is None:
                loose_at = len(out)
                out.append(None)
            loose.append(m.group(2))
            continue
        m = re.match(r'\s*(?:layout\s*\(\s*location\s*=\s*0\s*\)\s*)?out\s+(?:highp\s+)?vec4\s+(\w+)\s*;\s*$', line)
        if m and stage == 'f':
            out.append(f'layout(location=0) out vec4 {m.group(1)};')
            continue
        m = re.match(r'\s*(in|out)\s+((?:highp\s+|mediump\s+|flat\s+)*)(\w+)\s+(\w+)\s*;\s*$', line)
        if m and (m.group(1) == 'out') == (stage == 'v'):
            if m.group(4) not in VARYINGS:
                raise ValueError(f'Varying {m.group(4)} has no location; add it to VARYINGS')
            out.append(f'layout(location={VARYINGS.index(m.group(4))}) {m.group(1)} {m.group(2)}{m.group(3)} {m.group(4)};')
            continue
        m = re.match(r'\s*layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*in\s+(float|vec4)\s+(\w+)\s*;\s*$', line)
        if m and stage == 'v' and m.group(3) in UINT_INPUTS:
            raw = 'uint' if m.group(2) == 'float' else 'uvec4'
            out.append(f'layout(location={m.group(1)}) in {raw} {m.group(3)}_raw;')
            out.append(f'#define {m.group(3)} {m.group(2)}({m.group(3)}_raw)')
            continue
        if stage == 'v' and re.match(r'\s*void\s+main\s*\(\s*\)', line):
            line = line.replace('main', 'aurora_main', 1)
        out.append(line)
    if loose_at is not None:
        if f'View={BLOCKS["View"]}' in blocks and LOOSE_BINDING[stage] == BLOCKS['View']:
            raise ValueError('A stage with the View block has loose uniforms at its binding')
        members = ' '.join(f'vec4 {name};' for name in loose)
        out[loose_at] = f'layout(std140, set=0, binding={LOOSE_BINDING[stage]}) uniform AuroraLoose{stage.upper()} {{ {members} }};'
    text = '\n'.join(out).replace('gl_VertexID', 'gl_VertexIndex')
    if stage == 'v':
        text += POSITION
    return text, blocks, loose, textures


def convert(source, stage, glslang, naga):
    kind = 'vert' if stage == 'v' else 'frag'
    with tempfile.TemporaryDirectory(prefix='web-bank-') as scratch:
        scratch = Path(scratch)
        es = scratch / f'es.{kind}'
        es.write_bytes(source)
        pre = subprocess.run([glslang, '-E', '-S', kind, str(es)], capture_output=True, text=True)
        if pre.returncode:
            raise RuntimeError(pre.stdout + pre.stderr)
        text, blocks, loose, textures = vulkan_glsl(pre.stdout, stage)
        vulkan = scratch / f'vk.{kind}'
        vulkan.write_text(text)
        spirv = scratch / 'stage.spv'
        result = subprocess.run([glslang, '-V', '--target-env', 'vulkan1.0', '-S', kind, '-o', str(spirv), str(vulkan)],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        wgsl = scratch / 'stage.wgsl'
        result = subprocess.run([naga, '--keep-coordinate-space', str(spirv), str(wgsl)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        body = wgsl.read_text()
    uses = []
    for name, kind in textures:
        sampled = re.search(r'textureSample\w*\(\s*' + re.escape(name) + r'_t\b', body) is not None
        uses.append(f'{name}:{kind}:{"sample" if sampled else "fetch"}')
    header = (f'// mh3-blocks {" ".join(blocks)}\n// mh3-loose {" ".join(loose)}\n'
              f'// mh3-textures {" ".join(uses)}\n')
    return header + body


def write_if_changed(path, data):
    data = data.encode() if isinstance(data, str) else data
    if path.exists() and path.read_bytes() == data:
        return
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_bytes(data)
    os.replace(temporary, path)


def embed(entries, stages, output, versions):
    """Compress each stage with a shared dictionary; emit the tables."""
    with tempfile.TemporaryDirectory(prefix='web-bank-dict-') as scratch:
        scratch = Path(scratch)
        samples = []
        for index, stage in enumerate(stages):
            sample = scratch / f'{index}.wgsl'
            sample.write_bytes(stage)
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
        frames = [(compressed / f'{index}.wgsl.zst').read_bytes() for index in range(len(stages))]
        decoded = scratch / 'decoded'
        decoded.mkdir()
        subprocess.run(['zstd', '-q', '-d', '-D', str(dictionary), '--output-dir-flat', str(decoded),
                        *(str(compressed / f'{index}.wgsl.zst') for index in range(len(stages)))], check=True)
        for index, stage in enumerate(stages):
            if (decoded / f'{index}.wgsl').read_bytes() != stage:
                raise RuntimeError(f'zstd round trip changed {entries[index]["key"]}_{entries[index]["stage"]}')
    blob = bytearray(dictionary_bytes)
    tables = {stage: [] for stage, _ in STAGES}
    for entry, stage, frame in zip(entries, stages, frames):
        tables[entry['stage']].append((int(entry['key'], 16), len(blob), len(frame), len(stage)))
        blob += frame
    write_if_changed(output / 'shader_bank.bin', bytes(blob))
    text = ['// Generated by scripts/web-shader-bank.py; do not edit.',
            '#include "shader_bank.hpp"',
            '#pragma clang diagnostic ignored "-Wc23-extensions"',
            'namespace aurora::webgpu::shader_bank {',
            'namespace {',
            'alignas(16) const unsigned char Bytes[] = {',
            f'#embed "{(output / "shader_bank.bin").resolve()}"',
            '};',
            '} // namespace',
            'const unsigned char* const Blob = Bytes;',
            f'const uint32_t DictionaryBytes = {len(dictionary_bytes)};']
    summary = {'tools': versions, 'dictionary_bytes': len(dictionary_bytes), 'blob_bytes': len(blob), 'stages': {}}
    for stage, name in STAGES:
        rows = sorted(tables[stage])
        if any(a[0] == b[0] for a, b in zip(rows, rows[1:])):
            raise RuntimeError(f'Duplicate {name} key in the shader bank')
        text.append(f'const Stage {name}Stages[] = {{')
        text += [f'    {{0x{key:016x}ull, {offset}u, {packed}u, {size}u}},' for key, offset, packed, size in rows]
        text.append('};')
        text.append(f'const size_t {name}StageCount = {len(rows)};')
        summary['stages'][stage] = {'stages': len(rows), 'bytes': sum(r[3] for r in rows), 'compressed_bytes': sum(r[2] for r in rows)}
    text.append('} // namespace aurora::webgpu::shader_bank')
    write_if_changed(output / 'shader_bank_tables.cpp', '\n'.join(text) + '\n')
    write_if_changed(output / 'shader_bank.json', json.dumps(summary, indent=2) + '\n')
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('sources', type=Path, help='mh3-web-shader-bank output (glsl/, manifest.tsv)')
    parser.add_argument('output', type=Path, help='directory for the embedded bank files')
    parser.add_argument('--cache', type=Path, default=Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'mh3-web-shader-bank')
    parser.add_argument('--glslang', default='glslangValidator')
    parser.add_argument('--naga', default='naga')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    entries = manifest(args.sources)
    args.cache.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    identity, versions = tool_identity(args.glslang, args.naga)
    sources = [entry['source'].read_bytes() for entry in entries]
    names = [cache_name(identity, entry['stage'], source) for entry, source in zip(entries, sources)]
    missing = [index for index, name in enumerate(names) if not (args.cache / f'{name}.wgsl').exists()]
    failures = []

    def one(index):
        entry = entries[index]
        try:
            write_if_changed(args.cache / f'{names[index]}.wgsl', convert(sources[index], entry['stage'], args.glslang, args.naga))
        except Exception as error:
            failures.append(f'{entry["key"]}_{entry["stage"]} ({entry["name"]}): {error}')

    if missing:
        print(f'web-shader-bank: converting {len(missing)} stages to WGSL', flush=True)
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            list(pool.map(one, missing))
    if failures:
        raise RuntimeError('WGSL conversion failed for:\n  ' + '\n  '.join(failures[:20]))
    stages = [(args.cache / f'{name}.wgsl').read_bytes() for name in names]
    summary = embed(entries, stages, args.output, versions)
    counts = ', '.join(f'{s["stages"]} {stage}' for stage, s in summary['stages'].items())
    raw = sum(s['bytes'] for s in summary['stages'].values())
    print(f'web-shader-bank: {counts}; {raw} bytes of WGSL, {summary["blob_bytes"]} embedded; '
          f'{len(missing)} converted, {len(entries) - len(missing)} cached')


if __name__ == '__main__':
    main()
