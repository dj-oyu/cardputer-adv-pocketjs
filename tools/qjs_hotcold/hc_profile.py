"""The per-block profile of the hot/cold layout (docs/vm/qjs-hotcold.md sec.4).

    python hc_profile.py run <qemu log or FIFO> <r5sim.elf> <run.json> [--nm NM] [--chunk 60]
    python hc_profile.py merge <quickjs.c.obj.hc0.s> <profile.json> <run.json>... [--hot 0.5]

`run` reads a QEMU `-d exec,nochain,in_asm` log of an r5sim image built with
-DPOCKET_QJS_HOTCOLD=gen (hc_asm.py gen: a symbol in front of every basic
block) and counts, per block, the turns that ran it. A turn is what r5sim
brackets with r5_turn_begin / r5_turn_end (the guest's frame() plus its jobs
plus end_turn; the same window as tools/r5sim/icache.py). The log is read as
a stream, so it can be a FIFO and never touch the disk.

`merge` turns one or more runs into the profile `hc_asm.py use` takes: for
every function of the assembly, a hash of its instructions and one tier per
block:
    hot   in some run, in some stretch of --chunk consecutive turns, the
          block ran in at least --hot of the turns. One class per set of
          runs it is hot in (all of them first), unless --single
    warm  it ran in some turn, or before the first turn (evaluation, start)
    cold  no run reached it
Only functions with a hot block are written; the others are not laid out.
"""
import argparse, json, os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hc_asm  # noqa: E402

def run(a):
    out = subprocess.run([a.nm, '--defined-only', a.elf], capture_output=True, text=True, check=True).stdout
    at = {}                 # address -> symbols there (block symbols and functions)
    begin = end = None
    for line in out.splitlines():
        p = line.split()
        if len(p) != 3 or p[1] not in 'tT':
            continue
        ad = int(p[0], 16)
        if p[2] == 'r5_turn_begin':
            begin = ad
        elif p[2] == 'r5_turn_end':
            end = ad
        at.setdefault(ad, []).append(p[2])
    if begin is None or end is None:
        raise SystemExit('hc_profile: not an r5sim image (no r5_turn_begin / r5_turn_end)')

    trace = re.compile(r'^Trace \d+: (?:0x)?[0-9a-f]+ \[[0-9a-f]+/([0-9a-f]+)/')
    insn = re.compile(r'^0x([0-9a-f]+):')
    tb_syms = {}            # translation block start -> addresses in it that carry a symbol
    cur = None              # the block being listed by in_asm
    cur_start = None
    pre = set()             # blocks run before the first turn
    turns = []              # per turn, the set of translation blocks it ran
    seq = None
    with open(a.log, 'r', errors='replace') as fh:
        for line in fh:
            c = line[0]
            if c == 'T':
                m = trace.match(line)
                if not m:
                    continue
                pc = int(m.group(1), 16); cur = None
                if pc == begin:
                    seq = set()
                elif pc == end:
                    if seq is not None:
                        turns.append(seq)
                    seq = None
                elif seq is not None:
                    seq.add(pc)
                elif not turns:
                    pre.add(pc)
            elif c == 'I' and line.startswith('IN:'):
                cur = set(); cur_start = None
            elif c == '0' and cur is not None:
                m = insn.match(line)
                if m:
                    ad = int(m.group(1), 16)
                    if cur_start is None:
                        # The same start can be listed again, shorter: with
                        # -icount QEMU retranslates a block cut at the
                        # instruction where its budget ran out. Keep the
                        # union (the longest listing).
                        cur_start = ad; cur = tb_syms.setdefault(ad, set())
                    if ad in at:
                        cur.add(ad)
            elif c == '-':
                cur = None
    n = len(turns)
    hits = {}               # address -> list of turn indices
    for t, tbs in enumerate(turns):
        seen = set()
        for pc in tbs:
            for ad in tb_syms.get(pc, ()):
                seen.add(ad)
        for ad in seen:
            hits.setdefault(ad, []).append(t)
    pre_ad = set()
    for pc in pre:
        pre_ad.update(tb_syms.get(pc, ()))
    syms = {}
    for ad in set(hits) | pre_ad:
        ts = hits.get(ad, [])
        best = 0.0
        # stretches of `chunk` turns, aligned to the first turn; a short last
        # stretch counts against its own length
        per = {}
        for t in ts:
            per[t // a.chunk] = per.get(t // a.chunk, 0) + 1
        for c, k in per.items():
            size = min(a.chunk, n - c * a.chunk)
            if size >= a.chunk // 2:
                best = max(best, k / size)
        for name in at[ad]:
            syms[name] = [len(ts), round(best, 3), 1 if ad in pre_ad else 0]
    json.dump({'elf': os.path.basename(a.elf), 'turns': n, 'chunk': a.chunk, 'symbols': syms}, open(a.out, 'w'))
    print(f'hc_profile: {n} turns, {len(syms)} symbols reached -> {a.out}')

def merge(a):
    runs = [json.load(open(r)) for r in a.runs]
    lines = open(a.asm).read().split('\n')
    # A hot block's class is the set of runs it is hot in. Blocks hot in every
    # run come first, then the larger sets: an app then finds the blocks it
    # shares with the others together, and its own together, instead of
    # spread between another app's.
    sigs = sorted(range(1, 1 << len(runs)), key=lambda m: (-bin(m).count('1'), -m))
    if a.single:
        sigs = []
    klass = {m: i for i, m in enumerate(sigs)}
    hot = max(len(sigs), 1)
    if hot + 2 > len(hc_asm.DIGITS):
        raise SystemExit('hc_profile: too many runs for one digit a class; merge some first or pass --single')
    funcs = {}
    counts = {}
    for f in hc_asm.parse(lines):
        if not isinstance(f, hc_asm.Func):
            continue
        tiers = []
        for k in range(len(f.blocks)):
            name = f.name if k == 0 else hc_asm.sym(f.name, k)
            mask = 0; ran = False
            for i, r in enumerate(runs):
                s = r['symbols'].get(name)
                if s:
                    ran = True
                    if s[1] >= a.hot:
                        mask |= 1 << i
            tiers.append((klass[mask] if sigs else 0) if mask else hot if ran else hot + 1)
        if min(tiers) >= hot:
            continue
        for b, t in zip(f.blocks, tiers):
            counts[t] = counts.get(t, 0) + len(b.insns)
        funcs[f.name] = {'h': f.hash(), 't': ''.join(hc_asm.DIGITS[t] for t in tiers)}
    meta = {'runs': [{'elf': r['elf'], 'turns': r['turns']} for r in runs], 'chunk': runs[0]['chunk'], 'hot': a.hot,
            'hot_classes': hot, 'note': a.note or '',
            'classes': [[os.path.basename(os.path.dirname(os.path.abspath(a.runs[i]))) for i in range(len(runs)) if m >> i & 1]
                        for m in sigs]}
    with open(a.out, 'w') as fh:
        json.dump({'meta': meta, 'files': {'quickjs.c': funcs}}, fh, indent=0, sort_keys=True)
        fh.write('\n')
    print(f'hc_profile: {len(funcs)} functions with hot blocks; instructions by class '
          + ' '.join(f'{hc_asm.tier_name(t, hot)}={counts.get(t, 0)}' for t in range(hot + 2)) + f' -> {a.out}')

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('log'); r.add_argument('elf'); r.add_argument('out')
    r.add_argument('--nm', default='xtensa-esp32s3-elf-nm')
    r.add_argument('--chunk', type=int, default=60)
    m = sub.add_parser('merge')
    m.add_argument('asm'); m.add_argument('out'); m.add_argument('runs', nargs='+')
    m.add_argument('--hot', type=float, default=0.5)
    m.add_argument('--note')
    m.add_argument('--single', action='store_true', help='one hot class, not one per set of runs')
    a = ap.parse_args()
    run(a) if a.cmd == 'run' else merge(a)

if __name__ == '__main__':
    main()
