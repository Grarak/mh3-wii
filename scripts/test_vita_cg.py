#!/usr/bin/env python3
"""Check portable Cg conversion/capture against the Python oracle, without a GPU.

Usage: test_vita_cg.py /path/to/cg_shader_tests [capture/shaders ...]
Each supplied directory contributes unique GLSL stages. Use --output to retain
the generated Cg corpus for a separate Sony offline compiler validation.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location('vita_shaders', Path(__file__).with_name('vita-shaders.py'))
oracle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(oracle)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def tokens(source):
    # Retain preprocessor aliases separately; the expression lexer skips them.
    directives = [line.strip() for line in source.splitlines() if line.startswith('#')]
    return directives, [word for word, _, _ in oracle.cg_tokens(source)]


def check_capture(binary, directory):
    subprocess.run([binary, '--capture', str(directory)], check=True)
    shaders = directory / 'shaders'
    metadata = [json.loads(path.read_text()) for path in shaders.glob('*.origin.json')]
    require(len(metadata) == 2, 'expected two original alpha variants')
    require(len(list(shaders.glob('*.vert'))) == 3, 'original vertex payloads missing or duplicated')
    require(len(list(shaders.glob('*.frag'))) == 3, 'original fragment payloads missing or duplicated')
    require(len(list(shaders.glob('*.cg'))) == 3, 'normalized pair should have three shared Cg outputs')
    require(len(list(shaders.glob('*.cg-error.txt'))) == 1, 'unsupported layout diagnostic missing')
    require({value['label'] for value in metadata} == {'direct "geometry"', 'alpha two'}, 'label escaping failed')
    require({value['source_origin'] for value in metadata} == {
        'test/generator.cpp:42 model=7', 'test/generator.cpp:42 model=8'}, 'origin metadata missing')
    require(len({value['vita_pair'] for value in metadata}) == 1, 'alpha literals failed to share Cg identity')
    require({tuple(value['alpha_references']) for value in metadata} == {(1.0, 0.0), (2.0, 0.0)}, 'alpha values missing')
    for value in metadata:
        vertex = (shaders / value['vertex_glsl']).read_text()
        fragment = (shaders / value['fragment_glsl']).read_text()
        original = oracle.source_hash(vertex) + '-' + oracle.source_hash(fragment)
        normalized = oracle.parameterize_alpha_references(fragment)
        normalized_pair = oracle.source_hash(vertex) + '-' + oracle.source_hash(normalized)
        require(value['glsl_pair'] == original, 'capture lost actual GLSL identity')
        require(value['vita_pair'] == normalized_pair, 'capture has incorrect Vita identity')
        require(value['alpha_parameterized'] and value['generator_version'] == 4, 'generator metadata incorrect')
        require(tokens((shaders / value['vertex_cg']).read_text()) == tokens(oracle.convert(vertex, 'vertex')), 'captured vertex differs from oracle')
        fragment_cg = (shaders / value['fragment_cg']).read_text()
        require(tokens(fragment_cg) == tokens(oracle.convert(normalized, 'fragment')), 'captured fragment differs from oracle')
        require(tokens((shaders / value['scalar_fragment_cg']).read_text()) == tokens(oracle.scalar_fragment(fragment_cg)), 'captured scalar differs from oracle')
    rows = [line.split('\t') for line in (directory / 'events.tsv').read_text().splitlines()]
    require(sum(row[0] == 'shader_cg' for row in rows) == 2, 'repeated pair rewrote capture events')
    require(sum(row[0] == 'shader_cg_error' for row in rows) == 1, 'repeated rejection rewrote diagnostics')
    require(sum(row[0] == 'shader' for row in rows) == 4, 'unique origins were lost or repeatedly logged')
    require(any(row[0] == 'shader_origin' and row[-1] == 'test/other.cpp:8' for row in rows), 'additional source origin missing')


def check_corpus(binary, captures, directory):
    directory.mkdir(parents=True, exist_ok=True)
    seen, scalar_count = set(), 0
    for capture in captures:
        require(capture.is_dir(), 'capture directory does not exist: ' + str(capture))
        for path in sorted(capture.iterdir()):
            if path.suffix not in ('.vert', '.frag'):
                continue
            stage, suffix = ('vertex', 'v') if path.suffix == '.vert' else ('fragment', 'f')
            source = path.read_text()
            if stage == 'fragment':
                source = oracle.parameterize_alpha_references(source)
            digest = oracle.source_hash(source)
            if (stage, digest) in seen:
                continue
            seen.add((stage, digest))
            input_path = directory / (digest + '_' + suffix + '.glsl')
            output_path = input_path.with_suffix('.cg')
            input_path.write_text(source)
            subprocess.run([binary, '--convert', stage, str(input_path), str(output_path)], check=True)
            translated = output_path.read_text()
            require(tokens(translated) == tokens(oracle.convert(source, stage)), 'C++/Python Cg mismatch: ' + str(path))
            if stage == 'fragment':
                scalar_path = directory / (digest + '_r.cg')
                subprocess.run([binary, '--scalar', str(output_path), str(scalar_path)], check=True)
                require(tokens(scalar_path.read_text()) == tokens(oracle.scalar_fragment(translated)), 'C++/Python scalar mismatch: ' + str(path))
                scalar_count += 1
    return len(seen), scalar_count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('captures', nargs='*', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    binary = str(args.binary.resolve())
    subprocess.run([binary], check=True)
    with tempfile.TemporaryDirectory(prefix='mh3-cg-check-') as temporary:
        root = Path(temporary)
        check_capture(binary, root / 'capture')
        count, scalars = check_corpus(binary, args.captures, args.output or root / 'corpus')
    print(f'Capture metadata/deduplication passed; {count} unique GLSL stages and {scalars} scalar outputs match the Python oracle')


if __name__ == '__main__':
    main()
