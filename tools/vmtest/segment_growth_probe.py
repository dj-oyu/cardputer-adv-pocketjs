#!/usr/bin/env python3
"""Same-binary D42 sizing sweep; host numbers are not device measurements."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
CASES = ['closures', 'promise_chain', 'l2b_flat_calls', 'seg_generator_frames',
         'deep_recursion_device', 'bench_calls']
SIZES = [(256, 1024), (256, 2048), (512, 2048), (512, 4096),
         (1024, 4096), (4096, 4096)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--variant', default='o2')
    ap.add_argument('--out', type=Path, default=ROOT / '.cache/vmtest/segment-growth.json')
    args = ap.parse_args()
    runner = ROOT / '.cache/vmtest' / ('vmrun-' + args.variant)
    unit = runner.parent / 'segment-growth-check'
    subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-isystem',
                    str(ROOT / 'components/quickjs-ng/quickjs-ng'),
                    str(ROOT / 'tools/vmtest/segment_growth_check.c'), '-o', str(unit)], check=True)
    subprocess.run([str(unit)], check=True)
    rows = []
    for case in CASES:
        path = ROOT / 'tools/vmtest/corpus' / (case + '.js')
        profile = 'device' if case.endswith('_device') else 'host'
        baseline = None
        for sizes in [None] + SIZES:
            cmd = [str(runner), '--profile', profile, '--stats']
            if sizes:
                cmd += ['--vm-seg-growth', *map(str, sizes)]
            result = subprocess.run(cmd + [str(path)], text=True, capture_output=True, timeout=90)
            raw = result.stdout + result.stderr
            if 'Sanitizer' in raw or 'runtime error:' in raw:
                raise SystemExit(raw)
            semantic = '\n'.join(line for line in raw.splitlines() if not line.startswith('#info'))
            verdict = (result.returncode, semantic)
            if sizes is None:
                if result.returncode:
                    raise SystemExit(f'{case}: baseline did not complete\n{raw}')
                baseline = verdict
            elif verdict != baseline:
                raise SystemExit(f'{case} {sizes}: output mismatch\n{raw}')
            line = next((line for line in raw.splitlines() if line.startswith('#info vmstack ')), '')
            stats = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', line)}
            if not {'seg_first', 'seg_max', 'held_max', 'live_max', 'seg_mallocs'} <= stats.keys():
                raise SystemExit('missing segment measurements: ' + raw)
            if sizes and (stats['seg_first'], stats['seg_max']) != sizes:
                raise SystemExit('sizing request not applied: ' + line)
            rows.append(dict(case=case, profile=profile, sizes=sizes, exit=result.returncode, stats=stats))
            print(f'{case:24} {str(sizes):13} held={stats["held_max"]:7} '
                  f'live={stats["live_max"]:7} mallocs={stats["seg_mallocs"]}')
    for options in [['0', '4096'], ['17', '4096'], ['512', '4000'], ['512', '256']]:
        result = subprocess.run([str(runner), '--vm-seg-growth', *options,
                                 str(ROOT / 'tools/vmtest/corpus/closures.js')], capture_output=True)
        if result.returncode != 3 or b'invalid or unsupported' not in result.stderr:
            raise SystemExit('invalid sizing accepted')
    conflict = subprocess.run([str(runner), '--vm-seg-growth', '512', '4096',
                               '--vm-seg-size', '512',
                               str(ROOT / 'tools/vmtest/corpus/closures.js')], capture_output=True)
    if conflict.returncode != 3 or b'mutually exclusive' not in conflict.stderr:
        raise SystemExit('conflicting sizing modes accepted')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(dict(variant=args.variant,
        binary_sha256=hashlib.sha256(runner.read_bytes()).hexdigest(), rows=rows), indent=2) + '\n')
    print(f'SEGMENT_GROWTH_OK comparisons={len(CASES)*len(SIZES)} output={args.out}')


if __name__ == '__main__':
    main()
