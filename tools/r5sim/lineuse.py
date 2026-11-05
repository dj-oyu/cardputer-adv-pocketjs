"""How much of each fetched 32 B line a JS turn executes (docs/vm/turn-line-use.md).

turnset.py counts the lines a turn touches; this counts the bytes inside them
that the turn actually executes. The rest of each line was read from flash
for nothing -- the room a perfect packing of the executed code could give
back. The ratio is the "line use".

    python tools/r5sim/lineuse.py turns.pkl.gz build/r5sim.elf build/r5sim.map \
        --nm <nm> --objdump <objdump> [--top 25] [--tsv out.tsv]

Input is the same `icache.py extract` pickle and the r5sim image's own ELF and
map (own layout, as turnset.py). Instruction lengths come from the ELF's
disassembly, not from the gaps between trace addresses, so a block's last
instruction is exact too.

What it reports, per turn and averaged over the turns in the pickle:
- touched lines L, executed bytes U (each byte counted once per turn), use
  U / 32L, and the packed bound U / 32: the lines the turn would need
  if its executed bytes sat back to back. The bound ignores that code cannot
  be split arbitrarily (a basic block runs to its branch, functions keep
  their alignment), so it is an upper bound on what packing can save.
- the same split by bucket (turnset.py's RULES), by function, and by heat
  (block executions reaching the line per turn), plus a histogram of how many
  bytes of a line are used.
- "used in some turn": bytes executed in any turn of the window. A line whose
  unused bytes are used in other turns is not dead code, only code this turn
  did not need -- a layout cannot pack both.

A line is charged to the function owning most of its executed bytes in that
turn; its bytes count for that function's bucket.
"""
import argparse, bisect, collections, gzip, math, os, pickle, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from turnset import BUCKETS, bucket_of, in_flash, map_objects, short_obj, symbols  # noqa: E402

# objdump prints the raw bytes as one hex run: "42000004:\t004136  \tentry".
INSN_RE = re.compile(r'^\s*([0-9a-f]+):\t([0-9a-f]+)\s')

def insn_lengths(objdump, elf):
    """address -> length in bytes of every instruction in the flash text."""
    out = subprocess.run([objdump, '-d', elf], capture_output=True, text=True).stdout
    lens = {}
    for line in out.splitlines():
        m = INSN_RE.match(line)
        if m:
            a = int(m.group(1), 16)
            if in_flash(a):
                lens[a] = len(m.group(2)) // 2
    return lens

def guess_len(addr, nxt):
    # Not in the disassembly (data in text, a literal pool): take the gap to
    # the next instruction of the block, else the widest core instruction.
    return min(nxt - addr, 3) if nxt is not None and 0 < nxt - addr <= 4 else 3

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('turns'); ap.add_argument('elf'); ap.add_argument('map')
    ap.add_argument('--nm', default='xtensa-esp32s3-elf-nm')
    ap.add_argument('--objdump', default='xtensa-esp32s3-elf-objdump')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--tsv', help='per-function rows: bucket, function, lines/turn, used B/turn, use')
    ap.add_argument('--packed-model', action='store_true',
                    help='also replay the turns with every byte no turn runs removed (see below)')
    a = ap.parse_args()

    d = pickle.load(gzip.open(a.turns, 'rb'))
    syms = [s for s in symbols(a.nm, a.elf) if in_flash(s[0])]
    starts = [s[0] for s in syms]
    ranges = map_objects(a.map)
    rstarts = [r[0] for r in ranges]
    lens = insn_lengths(a.objdump, a.elf)

    def obj_of(addr):
        i = bisect.bisect_right(rstarts, addr) - 1
        return short_obj(ranges[i][2]) if i >= 0 and addr < ranges[i][0] + ranges[i][1] else '?'

    fn_cache = {}
    def fn_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        while i >= 0 and not (syms[i][0] <= addr < syms[i][0] + syms[i][1]):
            if addr - syms[i][0] > 0x40000:
                return ('?', 'libc/other')
            i -= 1
        if i < 0:
            return ('?', 'libc/other')
        s, z, n = syms[i]
        r = fn_cache.get(s)
        if r is None:
            r = fn_cache[s] = (n, bucket_of(n, obj_of(s)))
        return r

    # Per block: {line: (bitmask of executed bytes, function -> its bytes' mask)}.
    # Non-flash code (IRAM, ROM) costs no line and is left out.
    guessed = 0
    blk = []
    for addrs in d['blocks']:
        per = {}
        for k, x in enumerate(addrs):
            if not in_flash(x):
                continue
            n = lens.get(x)
            if n is None:
                n = guess_len(x, addrs[k + 1] if k + 1 < len(addrs) else None); guessed += 1
            f = fn_of(x)
            for b in range(x, x + n):
                l = b >> 5
                bit = 1 << (b & 31)
                m, fb = per.get(l, (0, None))
                if fb is None:
                    fb = collections.defaultdict(int)
                per[l] = (m | bit, fb)
                fb[f] |= bit
        blk.append([(l, m, fb) for l, (m, fb) in per.items()])

    n = len(d['turns'])
    tot_lines = tot_used = 0
    by_b = collections.defaultdict(lambda: [0, 0])          # bucket -> [lines, used bytes]
    by_f = collections.defaultdict(lambda: [0, 0])          # (fn, bucket) -> [lines, used bytes]
    hist = collections.Counter()                            # used bytes (1..32) -> lines
    reach = collections.Counter()                           # line -> block executions, all turns
    any_mask = collections.defaultdict(int)                 # line -> bytes used in any turn
    per_turn = []
    turn_lines = []
    for t in d['turns']:
        mask = collections.defaultdict(int); own = collections.defaultdict(lambda: collections.defaultdict(int))
        for bi in t:
            for l, m, fb in blk[bi]:
                reach[l] += 1
                mask[l] |= m
                for f, fm in fb.items():
                    own[l][f] |= fm
        lines = len(mask); used = 0
        for l, m in mask.items():
            u = bin(m).count('1'); used += u
            hist[u] += 1
            any_mask[l] |= m
            f = max(own[l].items(), key=lambda kv: bin(kv[1]).count('1'))[0]
            by_b[f[1]][0] += 1; by_b[f[1]][1] += u
            by_f[f][0] += 1; by_f[f][1] += u
        per_turn.append((lines, used))
        turn_lines.append(mask)
        tot_lines += lines; tot_used += used

    def kib(lines):
        return lines * 32 / 1024
    L, U = tot_lines / n, tot_used / n
    print(f'{a.elf}: {n} turns (from {d["first"]}), own layout, 32 B lines'
          + (f'; {guessed} instruction fetches had no disassembly (length guessed)' if guessed else ''))
    lo = min(u / (32 * l) for l, u in per_turn); hi = max(u / (32 * l) for l, u in per_turn)
    print(f'  per turn: lines {L:.0f} ({kib(L):.1f} KiB), executed {U:.0f} B ({U/1024:.1f} KiB), '
          f'use {U/(32*L):.1%} (turns {lo:.1%}..{hi:.1%})')
    print(f'  packed bound: {math.ceil(U/32)} lines ({U/1024:.1f} KiB), i.e. at most '
          f'{L - U/32:.0f} lines ({kib(L - U/32):.1f} KiB) of the touched lines are bytes this turn did not run')
    # Bytes this turn fetched but did not run, split by whether any turn of
    # the window runs them.
    other = never = 0
    for mask in turn_lines:
        for l, m in mask.items():
            idle = ~m & 0xffffffff
            other += bin(idle & any_mask[l]).count('1')
            never += bin(idle & ~any_mask[l] & 0xffffffff).count('1')
    print(f'  unused bytes of touched lines/turn: {(other+never)/n:.0f} B, of which run in another turn '
          f'{other/n:.0f} B, run in no turn {never/n:.0f} B')

    print(f'\n  {"bucket":18s} {"KiB/turn":>9s} {"used KiB":>9s} {"use":>6s} {"waste KiB":>10s}')
    for b in BUCKETS:
        l, u = by_b[b]
        if l:
            print(f'  {b:18s} {kib(l/n):9.2f} {u/n/1024:9.2f} {u/(32*l):6.1%} {kib(l/n) - u/n/1024:10.2f}')

    print('\n  lines by executed bytes (lines/turn, cumulative share of touched lines):')
    acc = 0
    for lo_, hi_ in ((1, 4), (5, 8), (9, 12), (13, 16), (17, 20), (21, 24), (25, 28), (29, 31), (32, 32)):
        k = sum(hist[u] for u in range(lo_, hi_ + 1)) / n; acc += k
        print(f'    {lo_:2d}..{hi_:2d} B  {k:7.1f}  {acc / L:6.1%}')

    print('\n  by heat (block executions reaching the line per turn):')
    print(f'    {"reach":>10s} {"lines/turn":>10s} {"used KiB":>9s} {"use":>6s}')
    band = [(0, 1), (1, 4), (4, 16), (16, 64), (64, None)]
    for lo_, hi_ in band:
        ls = us = 0
        for mask in turn_lines:
            for l, m in mask.items():
                r = reach[l] / n
                if r >= lo_ and (hi_ is None or r < hi_):
                    ls += 1; us += bin(m).count('1')
        if ls:
            name = f'{lo_}..{hi_}' if hi_ else f'{lo_}+'
            print(f'    {name:>10s} {ls/n:10.1f} {us/n/1024:9.2f} {us/(32*ls):6.1%}')

    print(f'\n  top {a.top} functions by touched lines/turn:')
    print(f'    {"KiB":>6s} {"used":>6s} {"use":>6s}  function  bucket')
    rows = sorted(by_f.items(), key=lambda kv: -kv[1][0])
    for (fn, b), (l, u) in rows[:a.top]:
        print(f'    {kib(l/n):6.2f} {u/n/1024:6.2f} {u/(32*l):6.1%}  {fn}  {b}')
    if a.packed_model:
        packed_model(d, blk, any_mask)
    if a.tsv:
        with open(a.tsv, 'w') as f:
            f.write('bucket\tfunction\tlines_per_turn\tused_B_per_turn\tuse\n')
            for (fn, b), (l, u) in rows:
                f.write(f'{b}\t{fn}\t{l/n:.1f}\t{u/n:.0f}\t{u/(32*l):.3f}\n')

class Cache:
    def __init__(self, sets, ways):
        self.sets, self.ways = sets, ways
        self.flush()
    def flush(self):
        self.lru = [collections.OrderedDict() for _ in range(self.sets)]
    def access(self, line):
        s = self.lru[line % self.sets]
        if line in s:
            s.move_to_end(line)
            return False
        if len(s) >= self.ways:
            s.popitem(last=False)
        s[line] = None
        return True

def packed_model(d, blk, any_mask):
    """Misses with the window's never-run bytes squeezed out.

    Every byte some turn of the window runs keeps its order; every byte no
    turn runs is removed, so the code closes up. That is what a perfect
    hot/cold split at byte granularity, trained on this very window, would
    give: an upper bound, not a layout a compiler can produce (blocks keep
    their alignment, branches over removed code still need their bytes, and
    the split itself adds jumps)."""
    order = sorted(any_mask)
    newpos = {}
    at = 0
    for l in order:
        m = any_mask[l]
        for b in range(32):
            if m >> b & 1:
                newpos[(l << 5) | b] = at; at += 1
    def lines_of(bi, packed):
        out = []
        for l, m, _ in blk[bi]:
            if not packed:
                out.append(l); continue
            seen = set()
            for b in range(32):
                if m >> b & 1:
                    nl = newpos[(l << 5) | b] >> 5
                    if nl not in seen:
                        seen.add(nl); out.append(nl)
        return out
    res = {}
    for packed in (False, True):
        bl = [lines_of(i, packed) for i in range(len(blk))]
        for geo, (sets, ways) in (('8-way', (128, 8)), ('full', (1, 1024))):
            warm, cold = Cache(sets, ways), Cache(sets, ways)
            wm = cm = touched = 0
            for t in d['turns']:
                cold.flush(); u = set()
                for bi in t:
                    for l in bl[bi]:
                        u.add(l)
                        wm += warm.access(l); cm += cold.access(l)
                touched += len(u)
            n = len(d['turns'])
            res[(packed, geo)] = (touched / n, wm / n, cm / n)
    print('\n  packed model (bytes no turn runs removed; upper bound, see packed_model):')
    print(f'    {"layout":10s} {"geometry":8s} {"lines/turn":>10s} {"warm":>7s} {"cold":>7s}')
    for packed in (False, True):
        for geo in ('8-way', 'full'):
            t, w, c = res[(packed, geo)]
            print(f'    {"packed" if packed else "as built":10s} {geo:8s} {t:10.0f} {w:7.0f} {c:7.0f}')
    print(f'    packed image of the window: {at} B ({at / 1024:.1f} KiB) of code run in some turn')

if __name__ == '__main__':
    main()
