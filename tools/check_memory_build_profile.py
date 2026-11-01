#!/usr/bin/env python3
"""Verify effective firmware diagnostics, not just requested CMake defaults."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TRACE = 'KASANE_MEGADEMO_TRACE'
PHASE = 'KASANE_MEMORY_PHASE_ONLY'
METRICS = 'POCKET_GUEST_ALLOCATOR_METRICS'
PROFILES = {'normal': (False, False), 'trace': (True, False), 'phase': (True, True)}


def cache_values(path):
    values = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        match = re.fullmatch(r'([^/#][^:]*):([^=]+)=(.*)', line)
        if match:
            key, kind, value = match.groups()
            if key in (TRACE, PHASE):
                if kind != 'BOOL' or value not in ('ON', 'OFF') or key in values:
                    raise ValueError('ambiguous BOOL cache entry: ' + key)
                values[key] = value == 'ON'
    if set(values) != {TRACE, PHASE}:
        raise ValueError('missing diagnostic BOOL cache entries')
    return values


def defines(row):
    command = row.get('command')
    if command is None:
        command = ' '.join(row['arguments'])
    result = {}
    for match in re.finditer(r'(?<!\S)-([DU])\s*([A-Za-z_]\w*)(?:=([^\s]+))?', command):
        op, key, value = match.groups()
        if key not in (TRACE, PHASE, METRICS):
            continue
        if op != 'D' or key in result:
            raise ValueError('ambiguous compiler definition: ' + key)
        result[key] = value
    return result


def check(build, profile):
    expected = dict(zip((TRACE, PHASE), PROFILES[profile]))
    actual = cache_values(build / 'CMakeCache.txt')
    if actual != expected:
        raise ValueError(f'cache profile mismatch: expected {expected}, got {actual}')
    rows = json.loads((build / 'compile_commands.json').read_text(encoding='utf-8'))
    suffixes = {'app': '/main/app_session.c', 'guest': '/components/pocketjs_guest/src/guest.c'}
    selected = {}
    for name, suffix in suffixes.items():
        found = [row for row in rows if row['file'].replace('\\', '/').endswith(suffix)]
        if len(found) != 1:
            raise ValueError(f'expected one {name} compile command, got {len(found)}')
        selected[name] = defines(found[0])
    wanted = {'app': {key: '1' for key in (TRACE, PHASE) if expected[key]},
              'guest': {METRICS: '1'} if expected[TRACE] else {}}
    if selected != wanted:
        raise ValueError(f'compiler profile mismatch: expected {wanted}, got {selected}')
    return dict(profile=profile, cache=actual, defines=selected)


def self_test():
    with tempfile.TemporaryDirectory(prefix='memory-profile-') as temp:
        root = Path(temp)
        def fixture(profile, windows=False, arguments=False):
            trace, phase = PROFILES[profile]
            (root / 'CMakeCache.txt').write_text(
                f'{TRACE}:BOOL={"ON" if trace else "OFF"}\n{PHASE}:BOOL={"ON" if phase else "OFF"}\n')
            rows = []
            for suffix, flags in [('main/app_session.c', [TRACE] * trace + [PHASE] * phase),
                                  ('components/pocketjs_guest/src/guest.c', [METRICS] * trace)]:
                file = '/source/' + suffix
                if windows:
                    file = 'C:' + file.replace('/', '\\')
                args = ['cc'] + ['-D' + key + '=1' for key in flags] + ['-c', file]
                rows.append(dict(file=file, **({'arguments': args} if arguments else {'command': ' '.join(args)})))
            (root / 'compile_commands.json').write_text(json.dumps(rows))
            return rows
        positives = negatives = 0
        for profile in PROFILES:
            for windows in (False, True):
                fixture(profile, windows, windows)
                check(root, profile)
                positives += 1
        def rejected(profile):
            nonlocal negatives
            try:
                check(root, profile)
            except ValueError:
                negatives += 1
                return
            raise AssertionError('invalid build profile accepted')
        fixture('normal'); rejected('phase')
        fixture('phase'); rejected('normal')
        rows = fixture('phase'); rows[0]['command'] = 'cc -c x'
        (root / 'compile_commands.json').write_text(json.dumps(rows)); rejected('phase')
        rows = fixture('phase'); rows[1]['command'] += ' -U' + METRICS
        (root / 'compile_commands.json').write_text(json.dumps(rows)); rejected('phase')
        rows = fixture('phase'); rows[0]['command'] += ' -D' + TRACE + '=0'
        (root / 'compile_commands.json').write_text(json.dumps(rows)); rejected('phase')
        rows = fixture('phase'); rows.pop()
        (root / 'compile_commands.json').write_text(json.dumps(rows)); rejected('phase')
        rows = fixture('phase'); rows.append(rows[0])
        (root / 'compile_commands.json').write_text(json.dumps(rows)); rejected('phase')
        fixture('phase'); (root / 'CMakeCache.txt').write_text(f'{TRACE}:BOOL=ON\n')
        rejected('phase')

        # Use the actual option declarations with CMake: changing a source
        # default must not be confused with overriding an old OFF cache.
        source = (ROOT / 'main/CMakeLists.txt').read_text(encoding='utf-8')
        option_lines = []
        declared = {}
        for key in (TRACE, PHASE):
            found = re.findall(r'^option\(' + key + r' .*\)$', source, re.MULTILINE)
            if len(found) != 1:
                raise AssertionError('missing source option: ' + key)
            option_lines += found
            literal = re.search(r' (ON|OFF)\)$', found[0])
            if not literal:
                raise AssertionError('source option needs an explicit BOOL default: ' + key)
            declared[key] = literal.group(1) == 'ON'
        project = root / 'probe'; project.mkdir()
        (project / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.16)\nproject(ProfileProbe NONE)\n' + '\n'.join(option_lines) + '\n')
        cmake = shutil.which('cmake')
        if not cmake:
            raise RuntimeError('cmake is required for source/cache regression')
        def configure(folder, *flags):
            subprocess.run([cmake, '-G', 'Ninja', '-S', str(project), '-B', str(root / folder), *flags],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            return cache_values(root / folder / 'CMakeCache.txt')
        fresh = configure('fresh')
        assert fresh == declared, (fresh, declared)
        off = {TRACE: False, PHASE: False}
        assert configure('reused', '-D' + TRACE + ':BOOL=OFF', '-D' + PHASE + ':BOOL=OFF') == off
        assert configure('reused') == off
        assert configure('normal', '-D' + TRACE + ':BOOL=OFF', '-D' + PHASE + ':BOOL=OFF') == off
        assert configure('trace', '-D' + TRACE + ':BOOL=ON', '-D' + PHASE + ':BOOL=OFF') == {TRACE: True, PHASE: False}
        print('MEMORY_PROFILE_SELF_TEST_PASS', json.dumps(dict(positive=positives, negative=negatives,
                                                             source_defaults=fresh, reused_off_preserved=True)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--profile', choices=PROFILES)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.build_dir is None or args.profile is None:
        parser.error('--build-dir and --profile are required')
    else:
        print('MEMORY_BUILD_PROFILE_PASS', json.dumps(check(args.build_dir, args.profile)))


if __name__ == '__main__':
    main()
