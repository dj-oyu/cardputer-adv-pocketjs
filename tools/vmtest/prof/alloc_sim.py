"""Replay a STRESS_ALLOC_TRACE (tools/test_stress_app.c) against small-block
caches, for R3's design (docs/vm/allocator-cost.md).

    python3 alloc_sim.py TRACE [--from F] [--to F]

Prints the steady-state mix per frame (usable sizes, the share of each size
class) and, for caches of K blocks per class up to MAXC bytes, the share of
mallocs a cache would serve, the frees it would absorb, and the most bytes it
ever held. A cache here: free() of a block of usable size u <= MAXC pushes it
on class u's stack if that stack holds < K; malloc(u) pops class u if it can.
realloc is left to the heap (its old block is freed to the heap, not cached).
"""
import argparse, collections

ap = argparse.ArgumentParser()
ap.add_argument('trace')
ap.add_argument('--from', dest='f0', type=int, default=30)
ap.add_argument('--to', dest='f1', type=int, default=299)
a = ap.parse_args()

ops = []          # (frame, op, fields)
frame = 0
for line in open(a.trace):
    p = line.split()
    if p[0] == 'F':
        frame = int(p[1]); continue
    ops.append((frame, p[0], p[1:]))

size_of = {}
mix = collections.Counter()
n_m = n_f = n_r = 0
for fr, op, f in ops:
    if op == 'm':
        size_of[f[1]] = int(f[0])
        if a.f0 <= fr <= a.f1: mix[int(f[0])] += 1; n_m += 1
    elif op == 'f':
        if a.f0 <= fr <= a.f1: n_f += 1
    elif op == 'r':
        size_of[f[1]] = int(f[2])
        if a.f0 <= fr <= a.f1: n_r += 1
frames = a.f1 - a.f0 + 1
print(f'frames {a.f0}..{a.f1}: per frame malloc {n_m/frames:.0f}, free {n_f/frames:.0f}, realloc {n_r/frames:.0f}')
tot = sum(mix.values()); acc = 0
print('usable size -> share of mallocs (cumulative):')
for u, c in sorted(mix.items(), key=lambda x: -x[1])[:14]:
    acc += c
    print(f'  {u:5d} B  {100*c/tot:5.1f}%  ({100*acc/tot:5.1f}%)')
small = sum(c for u, c in mix.items() if u <= 64)
print(f'mallocs <= 64 B: {100*small/tot:.1f}%   <= 128 B: {100*sum(c for u,c in mix.items() if u<=128)/tot:.1f}%')

print('\ncache simulation (steady-state frames only in the rates; bytes over the whole run):')
for maxc in (64, 128):
    for k in (4, 8, 16, 32):
        stacks = collections.defaultdict(list)
        live_size = {}
        hits = mallocs = absorbed = frees = 0
        held = peak = 0
        for fr, op, f in ops:
            steady = a.f0 <= fr <= a.f1
            if op == 'm':
                u = int(f[0]); live_size[f[1]] = u
                if steady: mallocs += 1
                s = stacks.get(u)
                if u <= maxc and s:
                    s.pop(); held -= u
                    if steady: hits += 1
            elif op == 'f':
                u = live_size.pop(f[0], None)
                if u is None: continue
                if steady: frees += 1
                if u <= maxc and len(stacks[u]) < k:
                    stacks[u].append(1); held += u; peak = max(peak, held)
                    if steady: absorbed += 1
            elif op == 'r':
                live_size.pop(f[0], None); live_size[f[1]] = int(f[2])
        print(f'  classes <= {maxc:3d} B, K={k:2d}: malloc hits {100*hits/max(mallocs,1):5.1f}%, '
              f'frees absorbed {100*absorbed/max(frees,1):5.1f}%, peak held {peak} B')

# A byte budget over the whole cache instead of K per class: a free is
# cached while the cache holds less than CAP bytes; only the listed classes.
def run_cap(classes, cap):
    stacks = collections.defaultdict(int)
    live_size = {}
    hits = mallocs = absorbed = frees = 0
    held = peak = 0
    for fr, op, f in ops:
        steady = a.f0 <= fr <= a.f1
        if op == 'm':
            u = int(f[0]); live_size[f[1]] = u
            if steady: mallocs += 1
            if u in classes and stacks[u]:
                stacks[u] -= 1; held -= u
                if steady: hits += 1
        elif op == 'f':
            u = live_size.pop(f[0], None)
            if u is None: continue
            if steady: frees += 1
            if u in classes and held + u <= cap:
                stacks[u] += 1; held += u; peak = max(peak, held)
                if steady: absorbed += 1
        elif op == 'r':
            live_size.pop(f[0], None); live_size[f[1]] = int(f[2])
    return 100 * hits / max(mallocs, 1), 100 * absorbed / max(frees, 1), peak

print('\nbyte-capped cache over chosen classes:')
top6 = {12, 16, 32, 36, 48, 80}
all64 = {u for u in range(12, 65, 4)}
for name, cls in (('top6', top6), ('all<=64', all64), ('all<=128', {u for u in range(12, 129, 4)})):
    for cap in (512, 1024, 2048, 4096):
        h, ab, pk = run_cap(cls, cap)
        print(f'  {name:9s} cap {cap:5d} B: malloc hits {h:5.1f}%, frees absorbed {ab:5.1f}%, peak {pk} B')
