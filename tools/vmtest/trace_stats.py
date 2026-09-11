#!/usr/bin/env python3
"""Replays a vmrun --trace file: validates it and summarises the allocation
history (the input L2a needs to pick a segment size, spec section 7).

    python3 tools/vmtest/trace_stats.py .cache/vmtest/traces/closures.trace

Replaying is the check that the format is self-consistent: every "-" and "~"
must name a live id, every id is new, and at "# end" nothing is live. Sizes
are the sizes QuickJS requested on the 64-bit host, not device sizes.
"""
import collections
import sys


def replay(path):
    live = {}  # id -> size
    peak_bytes = cur = 0
    peak_blocks = 0
    n = collections.Counter()
    sizes = collections.Counter()
    gcs = []
    last_id = 0
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            p = line.split()
            if not p:
                continue
            op = p[0]
            if op == "#":
                if len(p) > 1 and p[1] == "gc":
                    gcs.append((lineno, int(p[2]), int(p[3])))
                continue
            if op == "+":
                i, s = int(p[1]), int(p[2])
                assert i > last_id, f"{path}:{lineno}: id {i} not increasing"
                last_id = i
                live[i] = s
                cur += s
                sizes[s] += 1
                n["malloc"] += 1
            elif op == "-":
                i = int(p[1])
                assert i in live, f"{path}:{lineno}: free of dead id {i}"
                cur -= live.pop(i)
                n["free"] += 1
            elif op == "~":
                o, i, s = int(p[1]), int(p[2]), int(p[3])
                assert o in live, f"{path}:{lineno}: realloc of dead id {o}"
                assert i > last_id, f"{path}:{lineno}: id {i} not increasing"
                last_id = i
                cur += s - live.pop(o)
                live[i] = s
                sizes[s] += 1
                n["realloc"] += 1
            elif op in ("!", "!~"):
                n["fail"] += 1
            else:
                raise SystemExit(f"{path}:{lineno}: unknown record {line!r}")
            if cur > peak_bytes:
                peak_bytes = cur
            if len(live) > peak_blocks:
                peak_blocks = len(live)
    return live, cur, peak_bytes, peak_blocks, n, sizes, gcs


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for path in sys.argv[1:]:
        live, cur, pb, pk, n, sizes, gcs = replay(path)
        print(f"{path}")
        print(f"  mallocs={n['malloc']} reallocs={n['realloc']} frees={n['free']} fails={n['fail']}")
        print(f"  peak_live_bytes={pb} peak_live_blocks={pk} gc_runs={len(gcs)}")
        print(f"  leaked_at_end={len(live)} blocks / {cur} bytes")
        buckets = collections.Counter()
        for s, c in sizes.items():
            b = 1
            while b < s:
                b <<= 1
            buckets[b] += c
        print("  size histogram (<=2^k: count): " +
              " ".join(f"{b}:{buckets[b]}" for b in sorted(buckets)))
        if live:
            sys.exit(1)


if __name__ == "__main__":
    main()
