"""Where the never-run bytes of the touched lines are (docs/vm/qjs-hotcold.md sec.2).

lineuse.py says how much of each fetched 32 B line a turn executes; this says
which source lines the rest belongs to, so a person can decide what to move
out of the hot path by hand (GCC for Xtensa has no hot/cold partitioning).

    python tools/r5sim/coldmap.py <dir>[,<dir>...] --nm <nm> --objdump <objdump> \
        --addr2line <addr2line> [--obj quickjs.c.obj] [--top 30] [--min 6] [--fn NAME]

Each <dir> is an r5sim build directory holding turns.pkl.gz (icache.py
extract), r5sim.elf and r5sim.map. With several (STRESS and DERBY), a byte
counts as run when any workload runs it; a function is matched by name, and
its bytes by offset, so the directories must hold the same object code for
the functions of interest (they do for quickjs.c.obj: one compile, two apps).

Per function (most wasted bytes first): its size, the bytes some turn runs,
the lines some turn touches, and the "waste" -- the never-run bytes inside
touched lines. Then the never-run runs of at least --min bytes that sit in a
touched line, each with the source line of its first instruction (innermost
inline frame, and the outermost when they differ).

A run is "interior" (I) when run code follows it inside the function: moving
it out closes the gap. A "tail" (T) run already sits behind the last run byte
and costs at most the rest of one line.
"""
import argparse, bisect, collections, gzip, os, pickle, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from turnset import in_flash, map_objects, short_obj, symbols  # noqa: E402
from lineuse import insn_lengths, guess_len  # noqa: E402

def load(d, nm, objdump, obj):
    """name -> (start, size, bytearray run[offset], set of touched lines)."""
    t = pickle.load(gzip.open(os.path.join(d, 'turns.pkl.gz'), 'rb'))
    elf = os.path.join(d, 'r5sim.elf')
    syms = [s for s in symbols(nm, elf) if in_flash(s[0])]
    starts = [s[0] for s in syms]
    ranges = map_objects(os.path.join(d, 'r5sim.map'))
    rstarts = [r[0] for r in ranges]
    lens = insn_lengths(objdump, elf)
    used = set()
    for bi in set(b for turn in t['turns'] for b in turn):
        addrs = t['blocks'][bi]
        for k, x in enumerate(addrs):
            if not in_flash(x):
                continue
            n = lens.get(x) or guess_len(x, addrs[k + 1] if k + 1 < len(addrs) else None)
            used.update(range(x, x + n))
    lines = set(b >> 5 for b in used)
    out = {}
    seen = collections.Counter()
    for s, z, n in syms:
        i = bisect.bisect_right(rstarts, s) - 1
        o = short_obj(ranges[i][2]) if i >= 0 and s < ranges[i][0] + ranges[i][1] else '?'
        if obj and obj not in o:
            continue
        seen[n] += 1
        key = n if seen[n] == 1 else f'{n}#{seen[n]}'
        run = bytearray(z)
        for b in range(s, s + z):
            if b in used:
                run[b - s] = 1
        out[key] = (s, z, run, lines)
    return elf, out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs')
    ap.add_argument('--nm', default='xtensa-esp32s3-elf-nm')
    ap.add_argument('--objdump', default='xtensa-esp32s3-elf-objdump')
    ap.add_argument('--addr2line', default='xtensa-esp32s3-elf-addr2line')
    ap.add_argument('--obj', default='quickjs.c.obj')
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--min', type=int, default=6)
    ap.add_argument('--fn', action='append', default=[], help='only these functions, every run listed')
    a = ap.parse_args()

    loaded = [load(d, a.nm, a.objdump, a.obj) for d in a.dirs.split(',')]
    elf, base = loaded[0]
    rows = []
    for name, (s, z, run, lines) in base.items():
        touched = bytearray(z)          # byte sits in a line some workload touches
        for b in range(z):
            if (s + b) >> 5 in lines:
                touched[b] = 1
        for _, other in loaded[1:]:
            o = other.get(name)
            if not o or o[1] != z:
                continue
            for b in range(z):
                run[b] |= o[2][b]
                if (o[0] + b) >> 5 in o[3]:
                    touched[b] = 1
        nrun = sum(run)
        if not nrun:
            continue
        waste = sum(1 for b in range(z) if touched[b] and not run[b])
        last = max(b for b in range(z) if run[b])
        runs = []
        b = 0
        while b < z:
            if run[b] or not touched[b]:
                b += 1; continue
            e = b
            while e < z and not run[e]:
                e += 1
            # the whole never-run stretch, even where it leaves the touched lines
            st = b
            while st > 0 and not run[st - 1]:
                st -= 1
            runs.append((st, e - st, 'I' if e <= last else 'T'))
            b = e
        rows.append((waste, name, s, z, nrun, sum(touched) // 1, runs))
    rows.sort(reverse=True)
    if a.fn:
        rows = [r for r in rows if r[1] in a.fn]
    tw = sum(r[0] for r in rows)
    print(f'{a.obj or "all objects"}: {len(rows)} functions run, {tw} never-run bytes in touched lines '
          f'({tw / 1024:.2f} KiB) over {a.dirs}')
    print(f'{"waste":>6} {"size":>6} {"run":>6} {"touched":>7}  function')
    for waste, name, s, z, nrun, tb, runs in rows[:a.top]:
        print(f'{waste:6d} {z:6d} {nrun:6d} {tb:7d}  {name}')
    print()
    for waste, name, s, z, nrun, tb, runs in rows[:a.top]:
        sel = [r for r in runs if r[1] >= a.min or a.fn]
        if not sel:
            continue
        print(f'== {name}: size {z}, run {nrun}, waste {waste}')
        q = subprocess.run([a.addr2line, '-i', '-e', elf] + [hex(s + st) for st, _, _ in sel],
                           capture_output=True, text=True).stdout.splitlines()
        # -i prints the inline chain innermost first, one line per frame; without
        # -a the frames of consecutive addresses run together, so ask one by one
        # only when the count does not match.
        if len(q) != len(sel):
            q = []
            for st, _, _ in sel:
                o = subprocess.run([a.addr2line, '-i', '-e', elf, hex(s + st)],
                                   capture_output=True, text=True).stdout.split()
                o = [os.path.basename(x) for x in o]
                q.append(o[0] if len(o) == 1 or o[0] == o[-1] else f'{o[0]} < {o[-1]}')
        else:
            q = [os.path.basename(x) for x in q]
        for (st, n, kind), where in zip(sel, q):
            print(f'  +{st:5d} {n:5d} B {kind}  {where}')

if __name__ == '__main__':
    main()
