"""Reproduce the bounded A/C host study, then compare the same frame hashes."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]


def compare(folder: Path, frames: int) -> dict:
    a, c = [json.loads((folder / f'{v}-result.json').read_text()) for v in ('A', 'C')]
    rows = [(folder / f'{v}-pixels.csv').read_text().splitlines()[:frames] for v in ('A', 'C')]
    if not all(x['passed'] and x['sources_unchanged_during_run'] for x in (a, c)):
        raise RuntimeError('a variant failed or its source changed during execution')
    if a['binary_sha256'] != c['binary_sha256']:
        raise RuntimeError('variants did not use the same binary')
    if len(rows[0]) != frames or rows[0] != rows[1]:
        raise RuntimeError('A/C first-frame hash sequence differs')
    result = dict(kind='HOST SIMULATION', frames=frames, same_binary=True,
                  binary_sha256=a['binary_sha256'], frame_hash_sequences_equal=True,
                  sequence_sha256=hashlib.sha256(('\n'.join(rows[0])+'\n').encode()).hexdigest(),
                  sequence_encoding='first frame CSV rows, UTF-8 with LF including trailing LF',
                  unique_frame_hashes=len({line.split(',')[-1] for line in rows[0]}),
                  device_measurement=False, raw_packet_byte_parity='UNMEASURED')
    (folder / 'comparison.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--m32', action='store_true')
    p.add_argument('--output', type=Path, default=ROOT / '.cache/native-model-study/ci')
    p.add_argument('--frames', type=int, default=301)
    p.add_argument('--compare-only', action='store_true')
    args = p.parse_args()
    if not 1 <= args.frames <= 3600:
        p.error('frames must be in 1..3600')
    folder = args.output.resolve()
    if not args.compare_only:
        subprocess.run(['node', 'tools/ayatsuri/model/test_pack_model.cjs'], cwd=ROOT, check=True)
        command = [sys.executable, str(Path(__file__).with_name('run_model_study.py')),
                   '--output', str(folder), '--frames', str(args.frames)]
        if args.m32:
            command.append('--m32')
        subprocess.run([*command, '--variant', 'A'], cwd=ROOT, check=True)
        binary = folder / ('build-m32' if args.m32 else 'build-asan') / 'test-megademo-app'
        subprocess.run([*command, '--variant', 'C', '--reuse-binary', str(binary)], cwd=ROOT, check=True)
    print('MODEL_COMPARISON_PASS', json.dumps(compare(folder, args.frames), sort_keys=True))


if __name__ == '__main__':
    main()
