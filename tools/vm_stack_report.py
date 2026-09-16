"""Check paired depth/frame-address samples, never infer per-app use from HWM."""
import argparse
import json
from pathlib import Path


def check_curve(depths, frames, expected):
    if len(depths) != len(frames) or len(depths) < 3:
        raise ValueError('missing or mismatched depth/frame samples')
    if any(b <= a for a, b in zip(depths, depths[1:])):
        raise ValueError('depths must increase within one probe invocation')
    span = max(frames) - min(frames)
    slopes = [(a - b) / (y - x) for x, y, a, b in
              zip(depths, depths[1:], frames, frames[1:])]
    if expected == 'flat' and span > 64:
        raise ValueError(f'flat C-frame span unexpectedly grew: {span}')
    if expected == 'recur' and min(slopes) < 64:
        raise ValueError(f'recursive negative control did not grow: {slopes}')
    return {'depths': depths, 'span_bytes': span,
            'bytes_per_depth_min': min(slopes), 'bytes_per_depth_max': max(slopes)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--expect', choices=('flat', 'recur'), required=True)
    parser.add_argument('--curves', type=int, help='require this many complete probe invocations')
    args = parser.parse_args()
    count = 0
    for line in args.capture.read_text(encoding='utf-8').splitlines():
        record = json.loads(line)
        samples = record.get('samples', {})
        if not samples.get('depth'):
            continue
        if (record.get('ended_early') or record.get('depth_drop', 0) or
                record.get('static', {}).get('stack_scope') != 'task_lifetime'):
            raise ValueError('incomplete run or missing scope metadata')
        result = check_curve(samples['depth'], samples.get('depth_fp', []), args.expect)
        print(json.dumps({'rep': record['rep'], 'condition': record['condition'],
                          'hwm': samples.get('depth_hwm'), **result}))
        count += 1
    if not count:
        raise ValueError('no depth curves captured')
    if args.curves is not None and count != args.curves:
        raise ValueError(f'expected {args.curves} curves, captured {count}')
    print(f'STACK_CURVE_OK mode={args.expect} curves={count}')


if __name__ == '__main__':
    main()
