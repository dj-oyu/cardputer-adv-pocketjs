"""Validate the two-order device sizing matrix and summarize memory, not speed."""
import argparse
import json
from pathlib import Path

from vm_l0_capture import SEGMENT_SIZES


def complete_frames(row):
    return all(len(row.get('samples', {}).get(metric, [])) == row.get('frames')
               for metric in ('frame', 'call', 'drain', 'jobs'))


def merge_repairs(records, repairs):
    """Replace entire incomplete runs, never cherry-pick better measurements."""
    def key(row):
        return row['segment_apply']['policy'], row['letter'], row['rep']
    original = {}
    replacement = {}
    for row in records:
        original.setdefault(key(row), []).append(row)
    for row in repairs:
        replacement.setdefault(key(row), []).append(row)
    for run, rows in replacement.items():
        if run not in original or all(complete_frames(r) for r in original[run]):
            raise ValueError('repair must replace an existing incomplete run')
        if not all(complete_frames(r) for r in rows):
            raise ValueError('repair still lacks samples')
        if rows[0].get('static') != original[run][0].get('static'):
            raise ValueError('repair firmware metadata differs')
    return [r for run, rows in original.items() for r in replacement.get(run, rows)]


def summarize(records):
    groups = {}
    for row in records:
        applied = row.get('segment_apply') or {}
        policy = applied.get('policy')
        if policy not in range(6) or applied.get('result') != 0:
            raise ValueError('unverified policy')
        sizes = SEGMENT_SIZES[policy]
        segment = row.get('segment') or {}
        if (segment.get('seg_first'), segment.get('seg_max')) != sizes:
            raise ValueError('reported segment sizes differ')
        if (applied.get('first'), applied.get('max')) != sizes:
            raise ValueError('applied segment sizes differ')
        if row.get('ended_early') or any(row.get(k, 0) for k in ('lat_drop', 'drainrun_drop', 'depth_drop', 'sample_errors')):
            raise ValueError('incomplete/dropped run')
        for metric in ('frame', 'call', 'drain', 'jobs'):
            if len(row.get('samples', {}).get(metric, [])) != row.get('frames'):
                raise ValueError('missing frame samples')
        key = (policy, row['letter'], row['rep'])
        groups.setdefault(key, []).append(row)
    expected = {(p, app, rep) for p in range(6) for app in 'XABCDEF' for rep in range(2)}
    if set(groups) != expected:
        raise ValueError(f'matrix mismatch: missing={sorted(expected-set(groups))}, extra={sorted(set(groups)-expected)}')
    summary = []
    for (policy, app, rep), rows in sorted(groups.items()):
        if len(rows) < 2 or len({r['seq'] for r in rows}) != len(rows):
            raise ValueError('missing or duplicate sample windows')
        segment = rows[0]['segment']
        if any(r['segment'] != segment for r in rows):
            raise ValueError('inconsistent final segment report')
        summary.append(dict(policy=policy, app=app, rep=rep, windows=len(rows),
            held=segment['held_max'], live=segment['live_max'], frame=segment['frame_max'],
            depth=segment['depth_max'], mallocs=segment['seg_mallocs'], pushes=segment['pushes'],
            heap_free=min(r['heap_free_min'] for r in rows if r['heap_free_min']),
            largest=min(r['heap_largest_min'] for r in rows if r['heap_largest_min'])))
    return summary


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--directory', type=Path, default=Path('.cache/vm'))
    ap.add_argument('--pattern', default='segments-repeat-v2-*.jsonl')
    ap.add_argument('--out', type=Path, default=Path('.cache/vm/segments-repeat-v2-summary.json'))
    ap.add_argument('--repair-pattern', help='explicit replacement captures for incomplete runs only')
    args = ap.parse_args()
    files = sorted(args.directory.glob(args.pattern))
    records = [json.loads(line) for file in files for line in file.read_text().splitlines() if line]
    repair_files = sorted(args.directory.glob(args.repair_pattern)) if args.repair_pattern else []
    repairs = [json.loads(line) for file in repair_files for line in file.read_text().splitlines() if line]
    if repairs:
        records = merge_repairs(records, repairs)
    summary = summarize(records)
    for policy in range(6):
        for app in 'XABCDEF':
            pair = [r for r in summary if r['policy'] == policy and r['app'] == app]
            def span(key):
                values = [r[key] for r in pair]
                return str(min(values)) if min(values) == max(values) else f'{min(values)}..{max(values)}'
            print(f'{policy} {app} held={span("held")} live={span("live")} '
                  f'frame={span("frame")} mallocs={span("mallocs")} '
                  f'free={span("heap_free")} largest={span("largest")}')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(dict(inputs=[str(p) for p in files],
        repairs=[str(p) for p in repair_files], runs=summary), indent=2)+'\n')
    print(f'DEVICE_SEGMENTS_OK runs={len(summary)} windows={len(records)}')


if __name__ == '__main__':
    main()
