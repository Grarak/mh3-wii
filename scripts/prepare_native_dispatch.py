#!/usr/bin/env python3
"""Refresh translator-owned dispatch, registration and direct-call traits.

This runs emit-build-shards only; it does not translate guest functions/assets.
scripts/translate.py runs it last, after the native index (CMake runs that
before loading shards.cmake). The runtime's sources reach the shards only
through generated/native_index.json, so the inputs digest that instead of every
source. The input and output digests keep unchanged configurations cheap and
detect edited/missing generated artifacts.
"""
from pathlib import Path
import argparse
import fcntl
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
NATIVE = re.compile(r'REGISTER_NATIVE_FUNCTION(?:_AS)?\s*\(\s*0x([0-9a-fA-F]{8})\s*,')
OVERRIDE = re.compile(r'PPC_NATIVE_OVERRIDE(?:_VOID)?\s*\(\s*([0-9a-fA-F]{8})\s*,')
FATAL = re.compile(r'GX_FATAL_STUB\s*\(\s*([0-9a-fA-F]{8})')
COMMENT_OR_LITERAL = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
STATIC_ENTRY = re.compile(r'\{0x([0-9a-fA-F]{8})u,\s*&')
TRAIT = re.compile(r'MKW_TRANSLATED_TRAIT\(([0-9a-fA-F]{8}),')


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def source_files(directory):
    return sorted(p for p in directory.rglob('*') if p.is_file() and p.suffix.lower() in ('.cpp', '.h', '.hpp'))


def native_addresses(directory):
    result = set()
    for p in source_files(directory):
        text = COMMENT_OR_LITERAL.sub('', p.read_text())
        for pattern in (NATIVE, OVERRIDE, FATAL):
            result.update(int(v, 16) for v in pattern.findall(text))
    return result


def validate(output, native, resolved_profile=None):
    """Check the native exclusions in both profiles and their direct traits."""
    manifest = output / 'shards.cmake'
    if not manifest.is_file():
        raise ValueError('missing generated shards.cmake')
    selected = {Path(p) for p in re.findall(r'"([^"\n]+\.(?:cpp|h|inc))"', manifest.read_text())}
    checked = {}
    higher_priority_mods = set()
    if resolved_profile:
        data = json.loads(resolved_profile.read_text())
        higher_priority_mods = {row['Address'] for row in data.get('Entries', [])
                               if row.get('Priority', 0) > 10000 and
                               row.get('Kind', '').lower() in ('base', 'mod', 'rr') and row.get('DirectCallAvailable')}
    for profile in ('base', 'retro_rewind'):
        tables = [p for p in selected if p.parent.name == profile + '_dispatch']
        if len(tables) != 1 or not tables[0].is_file():
            raise ValueError(f'{profile}: expected one selected indirect table')
        entries = {int(v, 16) for v in STATIC_ENTRY.findall(tables[0].read_text())}
        expected_native = native - higher_priority_mods if profile == 'retro_rewind' else native
        stale = sorted(expected_native & entries)
        if stale:
            raise ValueError(f'{profile}: stale native indirect winners: ' + ', '.join(f'{v:08X}' for v in stale))
        checked[profile] = dict(entries=len(entries), table=str(tables[0]), sha256=digest(tables[0]))
    # The trait header follows its selected cpp shard. Obsolete unselected
    # files cannot influence this gate or make a stale graph appear fresh.
    for p in selected:
        trait = p.with_name(p.stem + '_traits.h')
        if not trait.is_file():
            continue
        expected_native = native - higher_priority_mods if p.parent.name.startswith('retro_') else native
        stale = expected_native & {int(v, 16) for v in TRAIT.findall(trait.read_text())}
        if stale:
            raise ValueError(f'{trait}: stale native direct-call traits: ' + ', '.join(f'{v:08X}' for v in sorted(stale)))
    return checked


def output_digests(directory):
    return {str(p.relative_to(directory)): digest(p) for p in sorted(directory.rglob('*'))
            if p.is_file() and (p.suffix in ('.cpp', '.h', '.inc', '.cmake') or p.name == 'base_common_shard_map.json')}


def input_digests(args):
    paths = [args.native_index] if args.native_index.is_file() else source_files(args.native_dir)
    paths += [args.base_metadata, args.translator, Path(__file__).resolve()]
    # Core dependencies matter even when the CLI wrapper itself is unchanged.
    paths += sorted(args.translator.parent.glob('Translator*.dll'))
    metadata = json.loads(args.base_metadata.read_text())
    bundle = metadata.get('sourceBundlePath')
    if bundle:
        p = Path(bundle)
        paths.append(p if p.is_absolute() else args.base_metadata.parent / p)
    else:
        paths += sorted(args.functions.glob('*.cpp'))
    if args.resolved_profile:
        paths.append(args.resolved_profile)
    if args.retro_cpp_dir:
        paths += source_files(args.retro_cpp_dir)
        bundle = args.retro_cpp_dir.parent / 'translated_sources.bin'
        if bundle.is_file():
            paths.append(bundle)
    return {str(p.resolve()): digest(p) for p in sorted(set(paths))}


def prepare(args):
    args.out.mkdir(parents=True, exist_ok=True)
    with (args.out / '.native-dispatch.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        stamp = args.out / 'native_dispatch_freshness.json'
        inputs = input_digests(args)
        recipe = [str(args.base_metadata), str(args.functions), str(args.native_dir),
                  str(args.resolved_profile or ''), str(args.retro_cpp_dir or '')]
        previous = json.loads(stamp.read_text()) if stamp.is_file() else {}
        fresh = (previous.get('format') == 2 and previous.get('inputs') == inputs and
                 previous.get('recipe') == recipe and previous.get('outputs') == output_digests(args.out))
        native = native_addresses(args.native_dir)
        if fresh and not args.force:
            tables = validate(args.out, native, args.resolved_profile)
            print(f'Native dispatch fresh: {len(native)} literal native addresses; base + retro_rewind')
            return tables
        if args.check:
            raise ValueError('native dispatch inputs/artifacts are stale; configure or run prepare_native_dispatch.py')
        # A caller must identify the actual mod recipe rather than silently
        # replace a previously generated mod graph with the base-only recipe.
        manifest = args.out / 'shards.cmake'
        if manifest.is_file() and not args.resolved_profile and not args.retro_cpp_dir:
            text = manifest.read_text()
            match = re.search(r'set\(MKW_RETRO_REWIND_FUNCTION_COUNT (\d+)\)', text)
            sensitive = re.search(r'set\(MKW_PROFILE_SENSITIVE_TARGET_COUNT (\d+)\)', text)
            if (match and int(match[1])) or (sensitive and int(sensitive[1])):
                raise ValueError('existing mod profile requires explicit --resolved-profile/--retro-cpp-dir')
        command = [str(args.dotnet), str(args.translator), 'emit-build-shards', '--project', str(ROOT / 'recomp.yml'),
                   '--base-metadata', str(args.base_metadata), '--base-functions-dir', str(args.functions),
                   '--native-source-dir', str(args.native_dir), '--out', str(args.out)]
        if args.resolved_profile:
            command += ['--resolved-profile', str(args.resolved_profile)]
        if args.retro_cpp_dir:
            command += ['--retro-cpp-dir', str(args.retro_cpp_dir)]
        print('Refreshing native dispatch with existing translator', flush=True)
        subprocess.run(command, cwd=ROOT, check=True)
        # Reject a concurrently changed producer/translation input. The next
        # configure can rerun; no stale stamp is published for the mixed state.
        if input_digests(args) != inputs:
            raise ValueError('native dispatch inputs changed during emission; retry after source freeze')
        tables = validate(args.out, native, args.resolved_profile)
        temporary = stamp.with_suffix('.tmp')
        temporary.write_text(json.dumps(dict(format=2, inputs=inputs, recipe=recipe,
                                             outputs=output_digests(args.out), native_addresses=sorted(native),
                                             tables=tables), indent=2) + '\n')
        temporary.replace(stamp)
        return tables


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-dir', type=Path, default=ROOT / 'runtime/src')
    parser.add_argument('--native-index', type=Path, default=ROOT / 'generated/native_index.json')
    parser.add_argument('--base-metadata', type=Path, default=ROOT / 'generated/translation.json')
    parser.add_argument('--functions', type=Path, default=ROOT / 'generated/functions')
    parser.add_argument('--out', type=Path, default=ROOT / 'generated/build_shards')
    parser.add_argument('--translator', type=Path,
                        default=ROOT / 'toolchain/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll')
    parser.add_argument('--dotnet', type=Path, default=Path(os.environ.get('DOTNET_BIN', ROOT / 'tools/dotnet/dotnet')))
    parser.add_argument('--resolved-profile', type=Path)
    parser.add_argument('--retro-cpp-dir', type=Path)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--force', action='store_true')
    args = parser.parse_args()
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.resolve())
    try:
        prepare(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Native dispatch preparation failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
