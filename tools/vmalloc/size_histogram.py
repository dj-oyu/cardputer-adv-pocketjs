"""Request-size distribution of vmtest traces, the evidence for the slab size classes.

Counts every '+' and '~' request before '# teardown' (a realloc's new size
is a request like any other: the allocator has to hold it), rounded up to 8
bytes, and separately the bytes each size holds at the trace's live-bytes
peak. A size that is requested often but dies young matters for speed; a
size that is alive at the peak is what the pool has to fit.

  python3 tools/vmalloc/size_histogram.py .cache/vmtest/traces/*.trace
  python3 tools/vmalloc/size_histogram.py --classes 16,24,32 TRACES   # rounding waste of a class set

Host traces: sizeof(JSValue)=16, pointer 8 (see each '# vmtrace' header).
Device sizes are smaller, so classes chosen from this are host classes;
docs/vm/vm-ledger/08-slab-study.md says so where it uses them.
"""
import argparse
import bisect
from collections import Counter
from pathlib import Path


def ops(path):
    with open(path) as f:
        for line in f:
            c = line[:1]
            if c == '#':
                if line.startswith('# teardown'):
                    return
            elif c == '+':
                _, i, s = line.split()
                yield None, i, int(s)
            elif c == '-':
                yield line.split()[1], None, 0
            elif c == '~':
                _, o, n, s = line.split()
                yield o, n, int(s)


def scan(path):
    # Pass 1: request counts and the op index of the live-bytes peak.
    live, counts = {}, Counter()
    cur = peak = peak_at = 0
    for k, (old, new, size) in enumerate(ops(path)):
        if old is not None:
            cur -= live.pop(old)
        if new is not None:
            live[new] = size
            cur += size
            counts[size] += 1
        if cur > peak:
            peak, peak_at = cur, k
    # Pass 2: replay up to the peak and take the size mix alive there.
    live = {}
    for k, (old, new, size) in enumerate(ops(path)):
        if old is not None:
            live.pop(old)
        if new is not None:
            live[new] = size
        if k == peak_at:
            break
    snap = Counter()
    for s in live.values():
        snap[s] += s
    return counts, snap, peak


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--classes', help='comma-separated class sizes; report rounding waste')
    ap.add_argument('traces', type=Path, nargs='+')
    a = ap.parse_args()
    counts, peak_bytes, total_peak = Counter(), Counter(), 0
    for t in a.traces:
        c, s, p = scan(t)
        counts.update(c)
        peak_bytes.update(s)
        total_peak += p
    n = sum(counts.values())
    by8, pb8 = Counter(), Counter()
    for s, k in counts.items():
        by8[(s + 7) // 8 * 8] += k
    for s, b in peak_bytes.items():
        pb8[(s + 7) // 8 * 8] += b
    print(f'requests={n} sum_of_trace_peaks={total_peak}')
    print(' size8   count  cum%  bytes_at_peak  cum%   (rows >=0.1% of requests or >=0.5% of peak bytes)')
    cum = cumb = 0
    for sz in sorted(set(by8) | set(pb8)):
        cum += by8[sz]
        cumb += pb8[sz]
        if by8[sz] * 1000 >= n or pb8[sz] * 200 >= total_peak:
            print(f'{sz:6d} {by8[sz]:7d} {100*cum/n:5.1f} {pb8[sz]:13d} {100*cumb/max(total_peak,1):5.1f}')
    if a.classes:
        cls = sorted(int(x) for x in a.classes.split(','))
        waste = fit = above = 0
        pw = pfit = 0
        for s, k in counts.items():
            i = bisect.bisect_left(cls, s)
            if i == len(cls):
                above += k
                continue
            fit += k
            waste += (cls[i] - s) * k
        for s, b in peak_bytes.items():
            i = bisect.bisect_left(cls, s)
            if i < len(cls):
                pfit += b
                pw += (cls[i] - s) * (b // s)
        print(f'classes={cls}')
        print(f'  requests in a class {fit} ({100*fit/n:.1f}%), above {above}; mean rounding {waste/max(fit,1):.1f} B')
        print(f'  at peak: {100*pfit/max(total_peak,1):.1f}% of live bytes in a class, rounding {pw} B '
              f'({100*pw/max(pfit,1):.1f}% of those bytes)')


if __name__ == '__main__':
    main()
