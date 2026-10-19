"""Require every source file to remain identical across two actual IDF builds."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def snapshot(source, build):
    files = {}
    for path in sorted(source.rglob('*')):
        relative = path.relative_to(source)
        # Match the broker: exclude only Git metadata and explicit output.
        if relative.parts[0] == '.git' or path == build or build in path.parents:
            continue
        if path.is_symlink():
            raise ValueError('linked source input is unsupported')
        if path.is_file():
            files[relative.as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--project-version')
    parser.add_argument('--fixed-workload', action='store_true',
                        help='explicitly opt in to the four finite-fixture diagnostic modes')
    args = parser.parse_args()
    source, build = args.source.resolve(), args.build.resolve()
    if source == build or source.is_relative_to(build):
        raise ValueError('build output must not contain the source root')
    before = snapshot(source, build)
    command = [sys.executable, str(Path(os.environ['IDF_PATH'])/'tools/idf.py'),
               '-B', str(build), '-D', 'SDKCONFIG='+str(build/'sdkconfig')]
    if args.project_version:
        command += ['-D', 'PROJECT_VER='+args.project_version]
    if args.fixed_workload:
        command += ['-D', 'IDF_TARGET=esp32s3']
        for option in ('POCKET_DERBY_TELEMETRY', 'POCKET_DERBY_MEASURE',
                       'POCKET_DERBY_COMPARE_FIXED_HW', 'POCKET_DERBY_FIXED_WORKLOAD'):
            command += ['-D', option+'=ON']
    command += ['build']
    checks = []
    for stage in ('complete_build', 'incremental_build'):
        subprocess.run(command, cwd=source, check=True)
        after = snapshot(source, build)
        if before != after:
            raise ValueError(json.dumps(dict(stage=stage,
                added=sorted(after.keys()-before.keys()),
                removed=sorted(before.keys()-after.keys()),
                changed=sorted(k for k in before.keys() & after.keys() if before[k] != after[k]))))
        checks.append(dict(stage=stage, source_unchanged=True))
    report = dict(source_files=len(before),
                  source_snapshot_sha256=hashlib.sha256(json.dumps(before, sort_keys=True).encode()).hexdigest(),
                  git_metadata_present=(source/'.git').exists(), checks=checks)
    (build/'source-immutability.json').write_text(json.dumps(report, indent=2)+'\n')
    print('DERBY_SOURCE_IMMUTABILITY '+json.dumps(report, sort_keys=True))


if __name__ == '__main__':
    main()
