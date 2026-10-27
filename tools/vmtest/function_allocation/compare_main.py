#!/usr/bin/env python3
"""Run the existing corpus on pinned main and this proposal, byte for byte."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess

from run import ROOT, QJS, run

BASE = '05f472ecc9535b71aefbdd0ed73d8891dfb4aace'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant', choices=['o2', 'asan'], required=True)
    parser.add_argument('--rom', action='store_true')
    parser.add_argument('--strip', action='store_true')
    args = parser.parse_args()
    variant = args.variant + ('-rom' if args.rom else '')
    tag = variant + ('-strip' if args.strip else '-debug')
    outputs = []
    names = {p.stem for p in (ROOT / 'tools/vmtest/corpus').glob('*.js')}
    for mode in ['main', 'candidate']:
        label = f'compare-{mode}-{tag}'
        directory = ROOT / '.cache/function_allocation' / label
        source = directory / 'source'
        shutil.copytree(QJS, source, dirs_exist_ok=True)
        if mode == 'main':
            for name in ['quickjs.c', 'quickjs-vmstack.h', 'quickjs-rom-intrinsics.h']:
                path = QJS.relative_to(ROOT) / name
                (source / name).write_bytes(subprocess.check_output(
                    ['git', 'show', f'{BASE}:{path}'], cwd=ROOT))
        env = dict(os.environ, VMTEST_QJS=str(source), VMTEST_OUT=str(directory),
                   VMTEST_CFLAGS='-DCONFIG_POCKET_VM_STRIP_DEBUG=1' if args.strip else '')
        run(label + '-build', ['bash', 'tools/vmtest/build.sh', variant], env)
        log = run(label + '-corpus', ['bash', 'tools/vmtest/run.sh', '--variant', variant],
                  env, allow_expected_difference=args.strip or args.rom)
        actual = directory / ('actual-' + variant)
        assert {p.stem for p in actual.glob('*.txt')} == names
        for name in names:
            data = (actual / (name + '.txt')).read_bytes()
            assert not re.search(rb'(?:Address|Leak|UndefinedBehavior|Memory)Sanitizer|runtime error:', data), (label, name)
            status = int(re.search(rb'exit=(\d+)\s*$', data).group(1))
            assert status < 128 and status != 124, (label, name, status)
        failures = sorted(re.findall(r'^FAIL (\S+)', log.read_text(), re.M))
        print(f'{label}: shared expectation differences={failures}', flush=True)
        metadata = directory / 'metadata.js'
        metadata.write_text('\nfunction positioned() {}\nprint(positioned.lineNumber, positioned.columnNumber);\n')
        result = subprocess.run([str(directory / ('vmrun-' + variant)), '--profile', 'host', str(metadata)],
                                cwd=ROOT, env=env, capture_output=True)
        (ROOT / '.cache/function_allocation/evidence' / (label + '-metadata.log')).write_bytes(
            result.stdout + result.stderr)
        assert result.returncode == 0 and result.stdout == b'2 1\n', (label, result)
        assert not re.search(rb'(?:Address|Leak|UndefinedBehavior|Memory)Sanitizer|runtime error:', result.stderr), (label, result)
        outputs.append(actual)
    for name in sorted(names):
        assert (outputs[0] / (name + '.txt')).read_bytes() == (outputs[1] / (name + '.txt')).read_bytes(), (tag, name)
    print(f'{tag}: {len(names)} files identical to main {BASE}; no sanitizer diagnostics', flush=True)


if __name__ == '__main__':
    main()
