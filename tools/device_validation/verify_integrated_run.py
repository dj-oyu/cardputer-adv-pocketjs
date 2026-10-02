"""Read-only preflight for Build-Integrated v1 artifacts; never opens a device.

Require the caller's exact commit, all recorded artifacts, generated config and
the post-build QuickJS response-file evidence. This is an integrity check of a
local build record, not signed provenance or proof of what is running on a board.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from verify_quickjs_flags import verify as verify_flags

ARTIFACTS = ('cardputer_pocketjs.bin', 'cardputer_pocketjs.elf',
             'cardputer_pocketjs.map', 'sdkconfig', 'CMakeCache.txt',
             'compile_commands.json')
APP_BYTES = 0x300000


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def text(path):
    raw = path.read_bytes()
    # Windows PowerShell 5.1 Tee-Object logs are UTF-16; PS7 logs are UTF-8.
    encoding = 'utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'
    return raw.decode(encoding).replace('\r\n', '\n')


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON key: ' + key)
        result[key] = value
    return result


def read_json(path):
    return json.loads(text(path), object_pairs_hook=unique_object)


def plain_path(path):
    path = Path(path)
    require(path.is_absolute(), 'Recorded paths must be absolute')
    # Do not let a junction or symlink redirect a retained run after its build.
    for part in (path, *path.parents):
        if part.exists():
            info = part.lstat()
            require(not part.is_symlink() and not (getattr(info, 'st_file_attributes', 0) & 0x400),
                    'Symlinks/junctions are not accepted: ' + str(part))
    return path.resolve(strict=True)


def same_path(value, expected):
    require(isinstance(value, str), 'Missing recorded path')
    require(plain_path(value) == expected, 'Unexpected artifact path: ' + value)


def hash_matches(path, expected):
    require(isinstance(expected, str) and re.fullmatch(r'[0-9a-fA-F]{64}', expected),
            'Missing or malformed SHA256: ' + str(path))
    require(path.is_file() and digest(path) == expected.lower(),
            'Artifact changed or missing: ' + str(path))


def git(worktree, *args):
    # Read-only commands; disable optional fsmonitor execution and index writes.
    # -C does not override GIT_DIR/GIT_WORK_TREE or injected GIT_CONFIG_*.
    # Retain ordinary PATH/tool resolution but remove every Git selector.
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith('GIT_')}
    env['GIT_OPTIONAL_LOCKS'] = '0'
    return subprocess.run(['git', '-c', 'core.fsmonitor=false', '-C', str(worktree), *args],
                          check=True, capture_output=True, text=True, env=env).stdout.strip()


def verify_run(root, commit):
    require(isinstance(commit, str) and re.fullmatch(r'[0-9a-fA-F]{40}', commit),
            'Expected a full 40-character commit SHA')
    commit = commit.lower()
    root = plain_path(root)
    manifest_path = plain_path(root / 'manifest.json')
    run = read_json(manifest_path)
    require(isinstance(run, dict) and run.get('schema') == 'pocketjs-integrated-build-v1'
            and run.get('manifestType') == 'integrated-build-only'
            and run.get('status') == 'built' and run.get('failure') is None,
            'A successfully built integrated v1 manifest is required')
    require(isinstance(run.get('commit'), str) and run['commit'].lower() == commit,
            'Manifest commit differs from -Commit')
    for field in ('typedPutEnabled', 'precompileExperimentEnabled', 'runtimePrecompileIntegrated'):
        require(run.get(field) is False, field + ' must be explicitly false (H/F OFF)')
    builds = run.get('builds')
    require(isinstance(builds, list) and len(builds) == 1, 'Exactly one integrated build is required')
    entry = builds[0]
    require(isinstance(entry, dict) and entry.get('role') == 'integrated'
            and entry.get('status') == 'built' and entry.get('configurationVerified') is True
            and isinstance(entry.get('sha'), str) and entry['sha'].lower() == commit,
            'Integrated artifact is incomplete, unverified or from a different commit')
    worktree = plain_path(root / 'wt-integrated')
    build = plain_path(worktree / 'build_integrated')
    same_path(entry.get('worktree'), worktree)
    same_path(entry.get('build'), build)
    require(entry.get('cmakeOptions') == [
        '-DSDKCONFIG=' + str(build) + '/sdkconfig',
        '-DSDKCONFIG_DEFAULTS=' + str(worktree) + '/sdkconfig.defaults',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DCMAKE_C_FLAGS=-DPOCKET_VM_TYPED_PUT_INT_FAST=0',
        '-DPOCKET_APP_PRECOMPILE_EXPERIMENT=OFF'], 'Unexpected integrated CMake options')
    records = entry.get('artifacts')
    require(isinstance(records, list) and len(records) == len(ARTIFACTS),
            'Complete six-artifact evidence is required')
    require(all(isinstance(item, dict) for item in records), 'Invalid artifact record')
    require(sorted(item.get('name', '') for item in records) == sorted(ARTIFACTS),
            'Missing, duplicate or unexpected artifact evidence')
    for item in records:
        path = plain_path(build / item['name'])
        same_path(item.get('path'), path)
        require(type(item.get('bytes')) is int and item['bytes'] == path.stat().st_size
                and item['bytes'] > 0, 'Artifact size mismatch: ' + item['name'])
        hash_matches(path, item.get('sha256'))
    binary = plain_path(build / 'cardputer_pocketjs.bin')
    same_path(entry.get('binary'), binary)
    require(type(entry.get('binaryBytes')) is int
            and 0 < entry['binaryBytes'] == binary.stat().st_size <= APP_BYTES,
            'Integrated image is empty, changed or exceeds the factory partition')
    hash_matches(binary, entry.get('binarySha256'))
    hash_matches(build / 'sdkconfig', entry.get('sdkconfigSha256'))
    dependency = entry.get('dependencyLock')
    require(isinstance(dependency, dict), 'Recorded dependency-lock evidence is required')
    lock = plain_path(worktree / 'dependencies.lock')
    same_path(dependency.get('path'), lock)
    hash_matches(lock, dependency.get('sha256'))

    require(git(worktree, 'rev-parse', 'HEAD').lower() == commit, 'Worktree HEAD changed')
    require(not git(worktree, 'status', '--porcelain', '--', '.', ':(exclude)dependencies.lock'),
            'Source worktree changed; do not flash a modified build run')
    partition = text(worktree / 'partitions.csv')
    require(len(re.findall(r'^factory,app,factory,0x10000,0x300000,\s*$', partition, re.M)) == 1,
            'Unexpected source factory partition layout')
    shell = text(worktree / 'main/ui/shell.c')
    app_ids = re.search(r'\bapp_ids\[\]\s*=\s*\{([^}]+)\}', shell)
    ids = re.findall(r'"([^"]+)"|\b(NULL)\b', app_ids[1]) if app_ids else []
    require(len(ids) == 14 and ids[0][0] == 'local.hello' and ids[9][0] == 'local.gridlab',
            'Embedded app navigation changed; this runner needs review')
    grid = text(worktree / 'apps/kasane/grid_lab.js')
    require('grid.measure(mode.handle, buffers, undefined, 8)' in grid,
            'Expected integrated scoped-input GRID API was not found')

    config = text(build / 'sdkconfig')
    require(re.search(r'^CONFIG_IDF_TARGET="esp32s3"$', config, re.M), 'Target must be ESP32-S3')
    require(not re.search(r'^CONFIG_(POCKET_VM_(SELFTEST|OOMPROBE|PROBE|RELOC|TURNPERF)|KSN_DEVICE_PROBE)=y$',
                          config, re.M), 'Diagnostic sdkconfig is not an ordinary image')
    cache = text(build / 'CMakeCache.txt')
    require(re.findall(r'^POCKET_APP_PRECOMPILE_EXPERIMENT:BOOL=(.*)$', cache, re.M) == ['OFF'],
            'F must be OFF in the generated CMake cache')
    for name, value in re.findall(r'^([A-Za-z0-9_]+):BOOL=(.*)$', cache, re.M):
        if (name.startswith('KASANE_') and name.endswith('_PROBE')) or name in ('POCKET_PROBES', 'POCKET_HEAPPROBE'):
            require(value in ('OFF', 'FALSE', '0', 'NO'), 'Diagnostic CMake option is enabled: ' + name)
    require(not list((build / 'experimental-bytecode').rglob('*.bc')), 'Unexpected F bytecode outputs')
    database = read_json(build / 'compile_commands.json')
    require(isinstance(database, list), 'Compilation database must be an array')
    evidence = verify_flags(database, 0)
    for response in evidence['responseFiles']:
        path = plain_path(response['path'])
        require(build in path.parents, 'QuickJS response file escaped the build directory')
    recorded = read_json(plain_path(root / 'integrated-build-flags.log'))
    require(recorded == evidence, 'QuickJS flag/response-file evidence changed after the build')
    return {'role': 'integrated', 'sourceSha': commit, 'binary': str(binary),
            'binarySha256': digest(binary), 'manifestSha256': digest(manifest_path),
            'worktree': str(worktree), 'build': str(build), 'runRoot': str(root),
            'configuration': 'H/F OFF; ordinary ESP32-S3 image',
            'status': 'preflight-verified'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-root', type=Path, required=True)
    parser.add_argument('--commit', required=True)
    args = parser.parse_args()
    try:
        result = verify_run(args.run_root, args.commit)
    except (OSError, ValueError, TypeError, KeyError, subprocess.SubprocessError) as error:
        print('Integrated preflight failed: ' + str(error), file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
