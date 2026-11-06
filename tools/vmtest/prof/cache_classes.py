"""Choose the R3 block cache's size classes from allocation traces
(docs/vm/qjs-observe.md sec.10).

    python3 tools/vmtest/prof/cache_classes.py TRACE [TRACE ...] [--depth 8]
        [--classes 12,16,24,...] [--from F] [--to F]

Traces are tools/test_stress_app.c's STRESS_ALLOC_TRACE format ("m <usable>
<id>", "f <id>", "r <old> <new> <usable>", "F <frame>"), from -m32 builds so
the sizes are the device's (tools/games/test_derby_host.c writes the same
with DERBY_ALLOC_TRACE). The replay is components/pocketjs_guest/src/
block_cache.c's rule, not alloc_sim.py's: at most 8 classes, a class is one
exact usable length, K blocks a class; malloc of a class length pops, free
pushes while the class holds < K; realloc goes to the heap.

Prints, per trace, the share of mallocs the shipped classes, the given
--classes and a greedy choice serve, and the most bytes the cache held
(outside the guest's accounting). The greedy choice adds, one at a time, the
length that raises the mean hit share over all traces the most.
"""
import argparse, collections

SHIPPED = (12, 16, 24, 28, 32, 36, 48, 80)   # guest.c; before 2026-11-06: 12,16,32,36,48,72,80,88
MAX_CLASSES = 8


def load(path):
    ops, frame = [], 0
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == 'F':
            frame = int(p[1])
            continue
        ops.append((frame, p[0], p[1:]))
    return ops


def replay(ops, classes, depth, f0, f1):
    classes = set(classes)
    count = collections.Counter()
    size_of = {}
    hits = mallocs = 0
    held = peak = 0
    for fr, op, f in ops:
        steady = f0 <= fr <= f1
        if op == 'm':
            u = int(f[0]); size_of[f[1]] = u
            if steady:
                mallocs += 1
            if u in classes and count[u]:
                count[u] -= 1; held -= u
                if steady:
                    hits += 1
        elif op == 'f':
            u = size_of.pop(f[0], None)
            if u is not None and u in classes and count[u] < depth:
                count[u] += 1; held += u; peak = max(peak, held)
        elif op == 'r':
            size_of.pop(f[0], None); size_of[f[1]] = int(f[2])
    return hits / max(mallocs, 1), peak, mallocs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('traces', nargs='+')
    ap.add_argument('--depth', type=int, default=8)
    ap.add_argument('--classes', default='12,16,32,36,48,72,80,88')   # the previous set
    ap.add_argument('--from', dest='f0', type=int, default=30)
    ap.add_argument('--to', dest='f1', type=int, default=10**9)
    a = ap.parse_args()
    runs = {t: load(t) for t in a.traces}
    # A trace with no frames (vmtrace2alloc.py writes one "F 1") counts whole.
    window = {t: ((a.f0, a.f1) if max((fr for fr, _, _ in ops), default=0) > 1 else (0, 10**9))
              for t, ops in runs.items()}
    mix = {}
    for t, ops in runs.items():
        f0, f1 = window[t]
        m = collections.Counter(int(f[0]) for fr, op, f in ops if op == 'm' and f0 <= fr <= f1)
        mix[t] = m
        tot = sum(m.values()) or 1
        top = ', '.join(f'{u}:{100 * c / tot:.1f}%' for u, c in m.most_common(12))
        print(f'{t}: {tot} mallocs in frames {f0}.., by usable size: {top}')
    given = tuple(int(x) for x in a.classes.split(','))
    # greedy over lengths that appear in any trace, mean hit share over traces
    lengths = sorted({u for m in mix.values() for u in m if u <= 252})
    chosen = []
    while len(chosen) < MAX_CLASSES:
        best = None
        for u in lengths:
            if u in chosen:
                continue
            score = sum(replay(ops, chosen + [u], a.depth, *window[t])[0] for t, ops in runs.items())
            if best is None or score > best[0]:
                best = (score, u)
        if best is None:
            break
        chosen.append(best[1])
    print(f'\ndepth {a.depth}; malloc hit share (peak bytes held)')
    sets = (('shipped', SHIPPED), ('given', given), ('greedy', tuple(sorted(chosen))))
    print(f"{'trace':40s} " + ' '.join(f'{n:>22s}' for n, _ in sets))
    for t, ops in runs.items():
        cells = []
        for n, cls in sets:
            h, pk, _ = replay(ops, cls, a.depth, *window[t])
            cells.append(f'{100 * h:6.1f}% ({pk:5d} B)')
        print(f'{t[-40:]:40s} ' + ' '.join(f'{c:>22s}' for c in cells))
    for n, cls in sets:
        print(f'  {n}: {",".join(map(str, cls))}')


if __name__ == '__main__':
    main()
