#!/usr/bin/env python3
"""Translate the game into generated/, running only the steps whose inputs changed.

CMake runs this at configure time (runtime/CMakeLists.txt) and re-runs it when
one of the inputs changes, so a build picks up an edit of symbols.txt,
recomp.yml, a site list, the translator's C# or a native port by itself;
scripts/translate.sh runs it by hand. Each step records a digest of its inputs
in generated/translation-state.json and runs again only when that changes
(--force runs every step):

  translator    dotnet build of toolchain/translator, from its C# and projects
  native index  the registrations, ABIs and guest effects of the runtime's
                native ports (generated/native_index.json): the runtime's
                sources reach the translation only through it, so an edit that
                leaves them alone translates nothing
  prepare       disc inventory, function seeds (build/MAP.txt), the data
                initializer and the boot RSOs' analysis image
  translate     the DOL into generated/functions (about 35 s)
  overlays      each RSO overlay profile into generated/rso_overlays (40-120 s
                each, OVERLAY_JOBS at a time)
  shards        the dispatch tables and build shards (prepare_native_dispatch.py)

The full output of each step goes to logs/. TRANSLATE_JOBS (default: all CPUs)
and OVERLAY_JOBS (default 5) set the parallelism; DOTNET_BIN and
WIICOMPILED_ROOT select the dotnet binary and the translator tree (default
toolchain/).
"""
from pathlib import Path
import argparse
import concurrent.futures
import fcntl
import hashlib
import json
import os
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from translate_rso_overlays import PROFILES  # noqa: E402

TOOLCHAIN = Path(os.environ.get('WIICOMPILED_ROOT', ROOT / 'toolchain'))
PROJECTS = (TOOLCHAIN / 'translator/src/Translator.Cli', TOOLCHAIN / 'translator/src/Translator.Core',
            TOOLCHAIN / 'Launcher/WiiCompiled.Setup.Common')
TRANSLATOR = TOOLCHAIN / 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
DOTNET = Path(os.environ.get('DOTNET_BIN', ROOT / 'tools/dotnet/dotnet'))
DATA = ROOT / 'Assets/DATA'
GENERATED = ROOT / 'generated'
STATE = GENERATED / 'translation-state.json'
NATIVE_INDEX = GENERATED / 'native_index.json'
LOGS = ROOT / 'logs'
FORMAT = 1

# The site lists recomp.yml names (the overlays filter tick_sites.inc themselves).
SITE_LISTS = [ROOT / 'runtime/src' / name for name in
              ('step_counter_sites.inc', 'tick_sites.inc', 'hardware_access.inc', 'alias_access.inc')]
PREPARE_SCRIPTS = [ROOT / 'scripts' / name for name in
                   ('inspect_disc.py', 'generate_symbols.py', 'prepare_rso_translation.py', 'link_rso.py',
                    'tick_sites_filter.py')]


def file_digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def digest(*parts):
    """One digest over files (by path and contents) and strings."""
    total = hashlib.sha256()

    for part in parts:
        if isinstance(part, Path):
            total.update(f'{part.relative_to(ROOT) if part.is_relative_to(ROOT) else part}\0'.encode())
            total.update((file_digest(part) if part.is_file() else 'missing').encode())
        else:
            total.update(f'{part}\0'.encode())

    return total.hexdigest()


def tree(directory, suffixes, skip=('bin', 'obj')):
    return sorted(p for p in directory.rglob('*') if p.is_file() and p.suffix in suffixes and
                  not set(p.relative_to(directory).parts) & set(skip))


def translator_sources():
    return [p for project in PROJECTS for p in tree(project, ('.cs', '.csproj', '.props', '.targets'))]


def translator_outputs():
    return sorted(TRANSLATOR.parent.glob('*.dll'))


def native_projection():
    """The native index without the C linkage, which only the shards use."""
    index = json.loads(NATIVE_INDEX.read_text())
    return json.dumps({key: index[key] for key in ('Registrations', 'VoidStubAbis', 'Effects')}, sort_keys=True)


def disc_files():
    files = [DATA / 'sys/main.dol', DATA / 'sys/boot.bin', DATA / 'files/mh3.sel']
    return files + sorted((DATA / 'files').glob('*/*.rso'))


class Steps:
    def __init__(self, force):
        self.force = force
        self.state = {}
        self.lock = threading.Lock()

        if STATE.is_file():
            state = json.loads(STATE.read_text())

            if state.get('format') == FORMAT:
                self.state = state['steps']

    def save(self):
        temporary = STATE.with_suffix('.tmp')
        temporary.write_text(json.dumps({'format': FORMAT, 'steps': self.state}, indent=2, sort_keys=True) + '\n')
        temporary.replace(STATE)

    def stale(self, name, inputs, outputs=()):
        return self.force or self.state.get(name) != inputs or not all(p.exists() for p in outputs)

    def begin(self, *names):
        # A step that fails or is interrupted runs again next time.
        with self.lock:
            for name in names:
                self.state.pop(name, None)

            self.save()

    def done(self, name, inputs):
        with self.lock:
            self.state[name] = inputs
            self.save()

    def run(self, name, inputs, outputs, action):
        if not self.stale(name, inputs, outputs):
            return False

        self.begin(name)
        started = time.monotonic()
        action()
        print(f'MH3 translation: {name} took {time.monotonic() - started:.0f} s', flush=True)
        self.done(name, inputs)
        return True


def run_logged(command, log, cwd=ROOT):
    """Runs `command` with its output in `log`; on failure shows its end."""
    LOGS.mkdir(exist_ok=True)

    with log.open('w') as out:
        result = subprocess.run([str(c) for c in command], cwd=cwd, stdout=out, stderr=subprocess.STDOUT)

    if result.returncode:
        tail = log.read_text(errors='replace').splitlines()[-15:]
        raise RuntimeError(f'{Path(command[0]).name} failed ({log.relative_to(ROOT)}):\n' + '\n'.join(tail))


def build_translator():
    run_logged([DOTNET, 'build', '-c', 'Release', '--nologo', '-v', 'q', '-clp:NoSummary', PROJECTS[0]],
               LOGS / 'translator-build.log')


def translator(*args):
    return [DOTNET, TRANSLATOR, *args]


def prepare():
    for script in ('inspect_disc.py', 'generate_symbols.py'):
        run_logged([sys.executable, ROOT / 'scripts' / script], LOGS / f'{Path(script).stem}.log')

    run_logged(translator('generate-data-init', '--project', ROOT / 'recomp.yml'), LOGS / 'data-init.log')
    run_logged([sys.executable, ROOT / 'scripts/prepare_rso_translation.py'], LOGS / 'prepare-rso.log')


def translate():
    jobs = os.environ.get('TRANSLATE_JOBS', str(os.cpu_count() or 4))
    run_logged(translator('translate-recursive', '0x80006310', '--project', ROOT / 'build/recomp-analysis.yml',
                          '--outdir', GENERATED / 'functions', '--output-metadata', GENERATED / 'translation.json',
                          '--threads', jobs, '--prune-stale'), LOGS / 'translate.log')


def overlays(steps, shared):
    stale = {}

    for profile, (name, *_) in PROFILES.items():
        inputs = digest(shared, DATA / f'files/01/{name}.rso')

        if steps.stale(f'overlay {profile}', inputs, [GENERATED / f'rso_overlays/{profile}.cpp']):
            stale[profile] = inputs

    if not stale:
        return

    def translate_overlay(profile):
        started = time.monotonic()
        run_logged([sys.executable, ROOT / 'scripts/translate_rso_overlays.py', '--profile', profile],
                   LOGS / f'{profile}-translate.log')
        return profile, time.monotonic() - started

    print(f'MH3 translation: {len(stale)} overlay profile(s): {" ".join(stale)}', flush=True)
    steps.begin(*(f'overlay {profile}' for profile in stale))
    jobs = int(os.environ.get('OVERLAY_JOBS', '5'))
    failures = []

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for future in concurrent.futures.as_completed([pool.submit(translate_overlay, p) for p in stale]):
            try:
                profile, seconds = future.result()
            except RuntimeError as error:
                failures.append(str(error))
                continue

            print(f'MH3 translation: overlay {profile} took {seconds:.0f} s', flush=True)
            steps.done(f'overlay {profile}', stale[profile])

    if failures:
        raise RuntimeError('\n'.join(failures))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--force', action='store_true', help='run every step')
    parser.add_argument('--resolved-profile', help='passed to prepare_native_dispatch.py')
    parser.add_argument('--retro-cpp-dir', help='passed to prepare_native_dispatch.py')
    args = parser.parse_args()

    if not (DATA / 'sys/main.dol').is_file():
        print(f'MH3 translation: {DATA.relative_to(ROOT)} is missing; extract the disc first '
              "(tools/nodtool extract -q 'IMAGE' Assets/DATA)", file=sys.stderr)
        return 1

    if not DOTNET.is_file():
        print(f'MH3 translation: no dotnet at {DOTNET} (the .NET 8 SDK; DOTNET_BIN names another)', file=sys.stderr)
        return 1

    GENERATED.mkdir(exist_ok=True)

    with (GENERATED / '.translation.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        steps = Steps(args.force)

        try:
            steps.run('translator', digest(*translator_sources()), [TRANSLATOR], build_translator)
            tools = digest(*translator_outputs())
            native_sources = tree(ROOT / 'runtime/src', ('.cpp', '.h', '.hpp'), skip=())
            steps.run('native index', digest(tools, *native_sources), [NATIVE_INDEX],
                      lambda: run_logged(translator('emit-native-index', '--native-source-dir', ROOT / 'runtime/src',
                                                    '--out', NATIVE_INDEX), LOGS / 'native-index.log'))
            natives = native_projection()
            steps.run('prepare', digest(tools, ROOT / 'symbols.txt', ROOT / 'recomp.yml', SITE_LISTS[1],
                                        *PREPARE_SCRIPTS, *disc_files()),
                      [ROOT / 'build/MAP.txt', ROOT / 'build/recomp-analysis.yml', GENERATED / 'rso_binding.h',
                       GENERATED / 'data_sections_init.cpp'], prepare)
            analysis = [ROOT / 'build/MAP.txt', ROOT / 'build/recomp-analysis.yml', ROOT / 'build/translation.dol',
                        ROOT / 'build/tick_sites-base.inc']
            steps.run('translate', digest(tools, natives, *analysis, *SITE_LISTS),
                      [GENERATED / 'translation.json'], translate)
            overlays(steps, digest(tools, natives, ROOT / 'recomp.yml', ROOT / 'symbols.txt', ROOT / 'build/MAP.txt',
                                   DATA / 'sys/main.dol', *SITE_LISTS, ROOT / 'scripts/translate_rso_overlays.py',
                                   *PREPARE_SCRIPTS))
        except RuntimeError as error:
            print(f'MH3 translation: {error}', file=sys.stderr)
            return 1

        # Its own digests (the native index, the generated functions) decide
        # whether it emits anything.
        shards = [sys.executable, ROOT / 'scripts/prepare_native_dispatch.py']

        if args.resolved_profile:
            shards += ['--resolved-profile', args.resolved_profile]

        if args.retro_cpp_dir:
            shards += ['--retro-cpp-dir', args.retro_cpp_dir]

        return subprocess.run([str(c) for c in shards], cwd=ROOT).returncode


if __name__ == '__main__':
    raise SystemExit(main())
