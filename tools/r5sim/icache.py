"""R5's instruction-cache model (docs/vm/r5-icache.md).

Replays the flash fetches of STRESS's JS turns, recorded by QEMU from the
r5sim image, through a model of the ESP32-S3 instruction cache, laid out at
the addresses of a firmware ELF.

    # once: the turns' fetch blocks out of the (~1 GB) QEMU log
    qemu-system-xtensa ... -d exec,nochain,in_asm -dfilter 0x42000000..0x43ffffff -D r5.log
    python tools/r5sim/icache.py extract r5.log tools/r5sim/build/r5sim.elf turns.pkl.gz
    # then any number of layouts and variants, in seconds
    python tools/r5sim/icache.py model turns.pkl.gz tools/r5sim/build/r5sim.elf build_x/cardputer_pocketjs.elf
        [--full]  [--iram-kib N]  [--pack-kib N]

What it rests on:
- A function's object code is the same in both images (same component, same
  flags: compare `nm -S` of the two quickjs.c.obj). The linked sizes differ by
  up to ~1.3% because the linker relaxes long calls by final distance, so an
  offset is carried across in proportion to the function's size -- inside a
  32-byte line, that is the whole error.
- The turn is cut by the two marker functions in r5sim.c, not by time.
- Rendering is not modelled (QEMU has no PIE; the image renders a 16x8
  scalar viewport only to consume submissions). The device renders between
  turns, so the truth lies between the two bounds this prints: "warm" (the
  cache survives from one turn to the next) and "cold" (every turn starts
  empty).
- Replacement is LRU. The TRM does not say what the S3 cache uses.
"""
import argparse, bisect, collections, gzip, pickle, re, subprocess, sys
from array import array

def symbols(nm, elf, flash_only=True):
    out = subprocess.run([nm, '-S', '--defined-only', elf], capture_output=True, text=True).stdout
    funcs = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in 'tTwW':
            a, n = int(p[0], 16), int(p[1], 16)
            if n and (not flash_only or 0x42000000 <= a < 0x44000000):
                funcs.append((a, n, p[3]))
    funcs.sort()
    return funcs

def in_flash(a):
    return 0x42000000 <= a < 0x44000000

def extract(a):
    sim = symbols(a.nm, a.sim_elf)
    begin = next(s for s, z, n in sim if n == 'r5_turn_begin')
    end = next(s for s, z, n in sim if n == 'r5_turn_end')
    tb_insns = {}        # block start -> instruction addresses
    ids = {}             # block start -> index into blocks
    blocks = []          # instruction addresses of each block used in a turn
    turns = []           # one array of block indices per turn in the window
    cur = None; turn = 0; seq = None
    # The host pointer is "0x..." from the Linux build, bare hex from Windows.
    trace = re.compile(r'^Trace \d+: (?:0x)?[0-9a-f]+ \[[0-9a-f]+/([0-9a-f]+)/')
    insn = re.compile(r'^0x([0-9a-f]+):')
    with open(a.log, 'r', errors='replace') as fh:
        for line in fh:
            c = line[0]
            if c == 'T':
                m = trace.match(line)
                if not m:
                    continue
                pc = int(m.group(1), 16); cur = None
                if pc == begin:
                    turn += 1
                    seq = array('I') if a.first <= turn <= a.last else None
                elif pc == end:
                    if seq is not None:
                        turns.append(seq)
                    seq = None
                elif seq is not None:
                    i = ids.get(pc)
                    if i is None:
                        addrs = tb_insns.get(pc)
                        if not addrs:
                            continue
                        i = ids[pc] = len(blocks); blocks.append(tuple(addrs))
                    seq.append(i)
            elif c == 'I' and line.startswith('IN:'):
                cur = []
            elif c == '0' and cur is not None:
                m = insn.match(line)
                if m:
                    ad = int(m.group(1), 16)
                    if not cur:
                        tb_insns[ad] = cur
                    cur.append(ad)
            elif c == '-':
                cur = None
    with gzip.open(a.out, 'wb') as f:
        pickle.dump({'first': a.first, 'blocks': blocks, 'turns': turns}, f)
    print(f'{len(turns)} turns, {len(blocks)} blocks, {sum(len(t) for t in turns)} executions -> {a.out}')

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

def model(a):
    d = pickle.load(gzip.open(a.turns, 'rb'))
    sim = symbols(a.nm, a.sim_elf)
    starts = [f[0] for f in sim]
    fw_all = symbols(a.nm, a.fw_elf, flash_only=False)
    fw = {n: (s, z) for s, z, n in fw_all if in_flash(s)}
    # Functions the firmware keeps in IRAM (a fragment's noflash): fetched
    # without the cache, so they cost no line.
    fw_iram = {n for s, z, n in fw_all if not in_flash(s) and 0x40370000 <= s < 0x403e0000}
    fw_end = max(s + z for s, z in fw.values())

    def locate(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return sim[i] if i >= 0 and addr < sim[i][0] + sim[i][1] else None

    # Firmware-relative placement of every executed instruction: (function,
    # offset in the firmware function). Unknown code keeps its sim address in
    # a region of its own.
    def place(addr):
        f = locate(addr)
        if f and f[2] in fw_iram:
            return f[2], 'iram'
        if f and f[2] in fw:
            s, z, n = f
            fs, fz = fw[n]
            return n, (addr - s) * fz // z
        return (f[2] if f else '?'), None

    blocks = []
    for addrs in d['blocks']:
        blocks.append([place(x) for x in list(addrs) + [addrs[-1] + 2]])

    # Per-function line use and first-pass miss attribution decide what the
    # variants move.
    touched = collections.defaultdict(set)
    for addrs in blocks:
        for n, off in addrs:
            if isinstance(off, int):
                touched[n].add(off >> 5)
    uses = collections.Counter()
    for t in d['turns']:
        for i in t:
            for n, off in blocks[i]:
                uses[n] += 1

    # Variant: the hottest functions (by fetches per byte) up to a budget.
    def pick(kib):
        ranked = sorted((n for n in touched if n in fw), key=lambda n: -uses[n] / fw[n][1])
        chosen, used = [], 0
        for n in ranked:
            if used + fw[n][1] > kib * 1024:
                continue
            chosen.append(n); used += fw[n][1]
        return chosen, used

    iram, iram_bytes = pick(a.iram_kib) if a.iram_kib else ([], 0)
    iram = set(iram)
    pack, pack_bytes = pick(a.pack_kib) if a.pack_kib else ([], 0)
    base = {}
    if pack:
        # Packed in one run past the end of the image: what a linker fragment
        # that lists them first would give, the rest of the image moving by a
        # constant (which leaves their relative placement, and so their
        # conflicts among themselves, as they were).
        at = (fw_end + 0x1000) & ~31
        for n in pack:
            base[n] = at; at += (fw[n][1] + 3) & ~3
    def line_of(n, off, raw):
        if off == 'iram':
            return None
        if off is None:
            return (raw | 0x80000000) >> 5
        if n in iram:
            return None
        return ((base[n] if n in base else fw[n][0]) + off) >> 5

    tb_lines = []
    for bi, addrs in enumerate(blocks):
        ls = []
        for (n, off), raw in zip(addrs, list(d['blocks'][bi]) + [d['blocks'][bi][-1] + 2]):
            l = line_of(n, off, raw)
            if l is not None and l not in ls:
                ls.append(l)
        tb_lines.append((tuple(ls), addrs[0][0]))

    sets, ways = (1, 1024) if a.full else (128, 8)
    warm, cold = Cache(sets, ways), Cache(sets, ways)
    per_turn = []; miss_by_fn = collections.Counter()
    for t in d['turns']:
        cold.flush(); uniq = set(); wm = cm = 0
        for i in t:
            ls, name = tb_lines[i]
            for l in ls:
                uniq.add(l)
                if warm.access(l):
                    wm += 1; miss_by_fn[name] += 1
                if cold.access(l):
                    cm += 1
        per_turn.append((wm, cm, len(uniq)))
    n = len(per_turn)
    avg = lambda k: sum(t[k] for t in per_turn) / n
    geo = 'fully associative 32 KiB' if a.full else '8-way 32 KiB'
    print(f'{a.fw_elf}: {geo}, turns {d["first"]}..{d["first"] + n - 1}')
    if iram:
        print(f'  IRAM: {len(iram)} functions, {iram_bytes} B of DRAM')
    if pack:
        print(f'  packed: {len(pack)} functions, {pack_bytes} B')
    if a.emit:
        with open(a.emit, 'w') as f:
            f.write(''.join(n + '\n' for n in (pack or sorted(iram))))
    print(f'  misses/turn  warm {avg(0):7.0f}   cold {avg(1):7.0f}   lines touched {avg(2):5.0f} ({avg(2) * 32 / 1024:.1f} KiB)')
    if a.top:
        tot = sum(miss_by_fn.values()) or 1
        for name, k in miss_by_fn.most_common(a.top):
            size = fw[name][1] if name in fw else 0
            print(f'    {k / n:7.1f}/turn {100 * k / tot:5.1f}%  touches {len(touched[name]) * 32:6d} of {size:6d} B  {name}')

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    e = sub.add_parser('extract')
    e.add_argument('log'); e.add_argument('sim_elf'); e.add_argument('out')
    e.add_argument('--first', type=int, default=61); e.add_argument('--last', type=int, default=120)
    m = sub.add_parser('model')
    m.add_argument('turns'); m.add_argument('sim_elf'); m.add_argument('fw_elf')
    m.add_argument('--full', action='store_true', help='fully associative: no conflict misses')
    m.add_argument('--iram-kib', type=float, default=0, help='take the hottest functions out of the cache up to this size')
    m.add_argument('--pack-kib', type=float, default=0, help='pack the hottest functions contiguously up to this size')
    m.add_argument('--top', type=int, default=0)
    m.add_argument('--emit', help='write the packed (or IRAM) function names here, one per line')
    for p in (e, m):
        p.add_argument('--nm', default='xtensa-esp32s3-elf-nm')
    a = ap.parse_args()
    extract(a) if a.cmd == 'extract' else model(a)

if __name__ == '__main__':
    main()
