"""Replay identical host traces; keep segment slack distinct from external holes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--pool', type=int, default=4 * 1024 * 1024)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('traces', type=Path, nargs='+')
args = parser.parse_args()
binary = root / '.cache/vmalloc/vmalloc_replay-o2'
report = {'pool': args.pool, 'sample_every': 1, 'host_replay': True,
          'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'runs': []}
failed = False
for trace in args.traces:
    digest = hashlib.sha256(trace.read_bytes()).hexdigest()
    for allocator in ('tlsf', 'estalloc', 'naive', 'segment'):
        output = subprocess.check_output([
            str(binary), '--allocator', allocator, '--pool', str(args.pool),
            '--sample-every', '1', '--verify', str(trace)], text=True)
        fields = dict(part.split('=', 1) for part in output.split() if '=' in part)
        failed |= fields.get('result') != 'OK' or fields.get('verify') != 'OK'
        report['runs'].append({'trace': str(trace), 'trace_sha256': digest, 'fields': fields})
        print(trace.stem, allocator, fields['result'],
              'external=' + fields['app_ext_frag'],
              'inside=' + fields.get('app_slack_inside', 'n/a'), flush=True)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
raise SystemExit(1 if failed else 0)
