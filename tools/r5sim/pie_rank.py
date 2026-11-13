"""Rank where the ESP32-S3's special instructions could pay off in a JS turn
(docs/vm/qjs-observe.md sec.8).

Reads observe.py's --json (every executed instruction with its count, and
every backward transfer inside one function) and ranks, by modelled cycles
saved per turn:

  PIE    loops whose body streams through memory -- copy, fill, search,
         reduce, element-wise map -- rewritten with 128-bit PIE vectors;
  LOOP   counted loops whose back edge is a taken branch (2 bubbles an
         iteration) that LOOP/LOOPNEZ/LOOPGTZ would make free;
  MOVcc  branches that skip one move, which MOVEQZ/MOVNEZ/MOVLTZ/MOVGEZ or
         MIN/MAX/MINU/MAXU turn into straight-line code;
  DIV    hardware divisions by a constant power of two (a shift would do).

and lists the hottest loops it rejected, with the reason (a call in the
body, pointer chasing, too few bytes per entry, ...).

It is a pattern matcher over the instructions QEMU executed, not a
vectoriser: a candidate says "this loop's instruction mix and trip count
would let PIE win by about this much", not that the code can be rewritten
as is (semantics, aliasing and alignment are the rewriter's to prove).
Every cycle figure is a model estimate:

  scalar  instructions + 2 per taken back edge + 1 per load-use pair
          (both measured on this board: CLAUDE.md, esp32s3-hw native_load_use)
  PIE     1 cycle per instruction, +0.6 per EE.VST.128.IP (measured,
          docs/perf/pie-simd.md), unaligned loads as EE.LD.128.USAR.IP +
          EE.SRC.Q (2), stores aligned by a scalar head; the head and tail
          (15 bytes on average together) and a fixed setup run scalar.

    python tools/r5sim/pie_rank.py obs.json [--pie-db esp32s3-hw-mcp/data/pie_instructions.json]
        [--top 30] [--min-cycles 200]
"""
import argparse, collections, json, os, re

LOADS = {'l8ui': 1, 'l16ui': 2, 'l16si': 2, 'l32i': 4, 'l32i.n': 4, 'lsi': 4, 'lsip': 4}
STORES = {'s8i': 1, 's16i': 2, 's32i': 4, 's32i.n': 4, 'ssi': 4, 'ssip': 4}
CALLS = re.compile(r'^call(x)?(0|4|8|12)$')
BRANCH = re.compile(r'^b(?!reak)')
REG = re.compile(r'^[af]\d+$')
ALU_PIE = {   # scalar op -> PIE op family (the lane width is the stream's)
    'add': 'EE.VADDS', 'add.n': 'EE.VADDS', 'addi': 'EE.VADDS', 'addi.n': 'EE.VADDS', 'addx2': 'EE.VADDS',
    'addx4': 'EE.VADDS', 'addx8': 'EE.VADDS', 'sub': 'EE.VSUBS', 'subx2': 'EE.VSUBS', 'subx4': 'EE.VSUBS',
    'and': 'EE.ANDQ', 'or': 'EE.ORQ', 'xor': 'EE.XORQ', 'extui': 'EE.ANDQ',
    'min': 'EE.VMIN', 'max': 'EE.VMAX', 'minu': 'EE.VMIN', 'maxu': 'EE.VMAX',
    'mul16u': 'EE.VMUL', 'mul16s': 'EE.VMUL', 'mull': 'EE.VMUL',
    'slli': 'EE.VSL.32', 'srli': 'EE.VSR.32', 'srai': 'EE.VSR.32', 'sll': 'EE.VSL.32', 'srl': 'EE.VSR.32',
    'sra': 'EE.VSR.32',
}
MOVES = {'mov', 'mov.n', 'movi', 'movi.n'}
MOVCC = {'beqz': 'MOVNEZ', 'beqz.n': 'MOVNEZ', 'bnez': 'MOVEQZ', 'bnez.n': 'MOVEQZ',
         'bltz': 'MOVGEZ', 'bgez': 'MOVLTZ'}            # skip-on-cond -> move-on-not-cond
MINMAX = {'blt': 'MIN/MAX', 'bge': 'MIN/MAX', 'bltu': 'MINU/MAXU', 'bgeu': 'MINU/MAXU'}
SETUP = 6          # PIE path: alignment test, loop setup, Q-register init (cycles, assumed)
TAIL_BYTES = 15    # scalar head (align the store) + tail, on average together (assumed)


def ops_of(text):
    return [o.strip() for o in text.split('\t', 1)[1].split(',')] if '\t' in text else []


def target_of(text):
    m = re.search(r'0x([0-9a-f]+)\s*$', text)
    return int(m.group(1), 16) if m else None


def load_pie_db(path):
    if not path or not os.path.exists(path):
        return {}
    return {i['name']: i.get('source_page') for i in json.load(open(path))}


class Body:
    """The executed instructions of one loop, in address order, and what they do."""

    def __init__(self, pcs, lo, hi, func, backedge_pc, entries_iters):
        self.ins = [(pc, *pcs[pc]) for pc in sorted(pcs) if lo <= pc <= hi and pcs[pc][4] == func]
        self.lo, self.hi, self.back = lo, hi, backedge_pc
        n_body = entries_iters
        self.dyn = sum(i[1] for i in self.ins)
        self.per_iter = self.dyn / max(n_body, 1)
        self.analyse()

    def analyse(self):
        defs = collections.defaultdict(list)       # reg -> mnemonics defining it in the body
        for pc, cnt, mn, text, ln, fn in self.ins:
            o = ops_of(text)
            if o and REG.match(o[0]) and mn not in STORES and not BRANCH.match(mn) and not CALLS.match(mn):
                defs[o[0]].append((mn, o))
        self.induction = {}
        for r, ds in defs.items():
            if len(ds) == 1 and ds[0][0] in ('addi', 'addi.n') and ds[0][1][1] == r:
                self.induction[r] = int(ds[0][1][2], 0)
            elif len(ds) == 1 and ds[0][0] in ('add', 'add.n') and r in ds[0][1][1:] and \
                    all(x == r or x not in defs for x in ds[0][1][1:]):
                self.induction[r] = None              # stride in an invariant register
        tainted = set()                               # registers holding (something derived from) a load
        for pc, cnt, mn, text, ln, fn in self.ins:
            o = ops_of(text)
            if mn in LOADS and o:
                tainted.add(o[0])
            elif o and REG.match(o[0]) and o[0] not in self.induction and mn not in STORES \
                    and not BRANCH.match(mn) and any(x in tainted for x in o[1:]):
                tainted.add(o[0])
        self.loads, self.stores, self.blockers = [], [], []
        self.alu = collections.Counter()
        self.exits = self.inner = self.calls = 0
        self.loaduse = 0
        prev = None
        for pc, cnt, mn, text, ln, fn in self.ins:
            o = ops_of(text)
            if mn in LOADS or mn in STORES:
                size = LOADS.get(mn) or STORES.get(mn)
                base = o[1] if len(o) > 1 else '?'
                if base == 'a1':
                    kind = 'stack'
                elif base in self.induction:
                    kind = 'stream'
                elif base in tainted:
                    kind = 'chase'
                elif base not in defs:
                    kind = 'invariant'
                else:
                    kind = 'computed'
                (self.loads if mn in LOADS else self.stores).append((kind, size, base, self.induction.get(base), cnt))
            elif CALLS.match(mn):
                self.calls += cnt
            elif BRANCH.match(mn) and pc != self.back:
                t = target_of(text)
                if t is not None and not (self.lo <= t <= self.hi):
                    self.exits += cnt
                else:
                    self.inner += cnt
            elif mn in ALU_PIE and not (o and o[0] in self.induction):
                self.alu[ALU_PIE[mn]] += cnt
            if prev and prev[2] in LOADS and prev[0] + prev[4] == pc:
                po = ops_of(prev[3])
                if po and po[0] in o[1:] + ([o[0]] if mn in STORES else []):
                    self.loaduse += min(cnt, prev[1])
            prev = (pc, cnt, mn, text, ln)

    def streams(self, which):
        return [x for x in which if x[0] == 'stream']


def classify(b):
    """(pattern, blocker or None)."""
    if b.calls:
        return None, 'call in the body'
    weight = lambda xs, k: sum(x[4] for x in xs if x[0] == k)
    chase = weight(b.loads, 'chase') + weight(b.stores, 'chase')
    comp = weight(b.loads, 'computed') + weight(b.stores, 'computed')
    sl, ss = b.streams(b.loads), b.streams(b.stores)
    if chase:
        return None, 'pointer chasing (address from a load)'
    if not sl and not ss:
        return None, 'no streaming memory access' + (' (computed addresses)' if comp else '')
    if comp > 0.2 * (sum(x[4] for x in sl + ss) or 1):
        return None, 'gather/scatter (computed addresses)'
    if b.inner > 0.25 * b.dyn / max(len(b.ins), 1):
        div = 'data-dependent branches inside the body'
    else:
        div = None
    if ss and not sl:
        return 'fill', div
    if sl and ss:
        return ('copy' if not b.alu else 'map'), div
    if b.exits:
        return 'search', div
    return 'reduce', div


def pie_cost(b, pattern, lanes_bytes=16):
    """Cycles per 16 bytes on the PIE path (model)."""
    sl, ss = b.streams(b.loads), b.streams(b.stores)
    nl = len({x[2] for x in sl}); ns = len({x[2] for x in ss})
    ops = len(b.alu)
    vec = 2 * nl + 1.6 * ns + ops
    if pattern == 'search':
        vec += 3        # EE.VCMP + EE.MOVI.32.A of the mask + a branch
    if pattern == 'fill':
        vec = 1.6 * ns
    return vec


SUGGEST = {
    'copy': ['EE.VLD.128.IP', 'EE.LD.128.USAR.IP', 'EE.SRC.Q', 'EE.VST.128.IP'],
    'fill': ['EE.ZERO.Q', 'EE.VLDBC.32', 'EE.VST.128.IP'],
    'search': ['EE.LD.128.USAR.IP', 'EE.SRC.Q', 'EE.VCMP.EQ.S8', 'EE.MOVI.32.A'],
    'reduce': ['EE.LD.128.USAR.IP', 'EE.VMULAS.S16.ACCX'],
    'map': ['EE.LD.128.USAR.IP', 'EE.SRC.Q', 'EE.VST.128.IP'],
}


def lane_name(fam, size):
    if fam in ('EE.ANDQ', 'EE.ORQ', 'EE.XORQ', 'EE.VSL.32', 'EE.VSR.32'):
        return fam
    return f'{fam}.S{8 * size}' if size in (1, 2, 4) else fam


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('json')
    ap.add_argument('--pie-db', default=os.path.join(os.path.dirname(__file__), '..', '..', '..',
                                                     'esp32s3-hw-mcp', 'data', 'pie_instructions.json'))
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--min-cycles', type=float, default=100)
    a = ap.parse_args()
    d = json.load(open(a.json))
    n = d['turns']
    pcs = {int(k, 16): v for k, v in d['pcs'].items()}   # pc -> [count, mn, text, len, func]
    db = load_pie_db(a.pie_db)
    total_cyc = (sum(d['insn_by_func'].values()) + 2 * sum(d['taken_by_func'].values())
                 + sum(d['loaduse_by_func'].values())
                 + 16 * sum(d['mnem'].get(m, 0) for m in ('quos', 'quou', 'rems', 'remu')))
    rows, rejected = [], []

    # ---- loops: PIE and LOOP
    edges = collections.defaultdict(lambda: [0, 'branch'])
    for b, t, k, v in d['backedges']:
        e = edges[(int(b, 16), int(t, 16))]; e[0] += v; e[1] = k
    for (bpc, tpc), (iters, kind) in edges.items():
        if bpc not in pcs or tpc not in pcs:
            continue
        func = pcs[bpc][4]
        entries = max(pcs[tpc][0] - iters, 1)
        runs = iters + entries
        trip = runs / entries
        if trip < 1.5:
            continue    # a backward jump to shared code, not a loop
        body = Body(pcs, tpc, bpc, func, bpc, runs)
        back_bubbles = 0 if kind == 'zloop' else 2     # branch or j back: 2 bubbles
        scalar_iter = body.per_iter + back_bubbles + body.loaduse / runs
        scalar_turn = scalar_iter * runs / n
        where = f'{func} @{tpc:#x}'
        rom = ' (ROM: replace by linking an own version)' if tpc < 0x40060000 else ''
        pattern, why = classify(body)
        sl, ss = body.streams(body.loads), body.streams(body.stores)
        strides = [abs(x[3]) for x in sl + ss if x[3]]
        bytes_iter = max(strides) if strides else 0
        if pattern and bytes_iter:
            size = min(x[1] for x in sl + ss)
            bytes_entry = trip * bytes_iter
            per_byte = scalar_iter / bytes_iter
            vec = pie_cost(body, pattern)
            if bytes_entry > 2 * TAIL_BYTES:
                pie_entry = SETUP + (bytes_entry - TAIL_BYTES) / 16 * vec + TAIL_BYTES * per_byte
            else:
                pie_entry = trip * scalar_iter
            save = (trip * scalar_iter - pie_entry) * entries / n
            insns = list(SUGGEST[pattern]) + sorted({lane_name(f, size) for f in body.alu})
            note = (why or '') + rom
            if save >= a.min_cycles:
                rows.append((save, 'PIE ' + pattern, where, trip, entries / n, bytes_entry, scalar_turn, insns, note))
            else:
                rejected.append((scalar_turn, where, trip, entries / n,
                                 f'{pattern}: {bytes_entry:.0f} B per entry is too few for 16 B vectors{rom}'))
        else:
            rejected.append((scalar_turn, where, trip, entries / n, (why or 'no stride') + rom))
        # LOOP: a counted loop closed by a taken branch; the back edge's
        # compare must read an induction register.
        counted = False
        if kind == 'branch' and not body.calls and not body.exits:
            # bottom-tested: the back edge compares the induction register
            counted = any(x in body.induction for x in ops_of(pcs[bpc][2]) if REG.match(x))
        elif kind == 'jump' and not body.calls and body.exits <= 1.05 * entries:
            # top-tested (j back): the one exit branch compares the induction register
            ex = [i for i in body.ins if BRANCH.match(i[2]) and i[0] != bpc and
                  (target_of(i[3]) is None or not (tpc <= target_of(i[3]) <= bpc))]
            counted = len(ex) == 1 and any(x in body.induction for x in ops_of(ex[0][3]) if REG.match(x))
        if counted:
            if True:
                save = (2 * iters - 3 * entries) / n
                if save >= a.min_cycles:
                    rows.append((save, 'LOOP', where, trip, entries / n, None, scalar_turn,
                                 ['LOOPNEZ', 'LOOPGTZ'], 'trip count must be known on entry' + rom))

    # ---- branches over one move: MOVcc / MIN / MAX
    for pc, (cnt, mn, text, ln, func) in pcs.items():
        if mn not in MOVCC and mn not in MINMAX:
            continue
        nxt = pcs.get(pc + ln)
        t = target_of(text)
        if not nxt or nxt[1] not in MOVES or t != pc + ln + nxt[3]:
            continue
        taken = max(cnt - nxt[0], 0)
        save = (2 * taken + nxt[0]) / n          # bubbles of the taken path + the move
        if save < a.min_cycles:
            continue
        if mn in MOVCC:
            rows.append((save, 'MOVcc', f'{func} @{pc:#x}', None, cnt / n, None, None, [MOVCC[mn]],
                         f'{mn} over {nxt[1]}'))
        else:
            bo, mo = ops_of(text), ops_of(nxt[2])
            if nxt[1] in ('mov', 'mov.n') and len(mo) > 1 and mo[1] in bo[:2]:
                rows.append((save, 'MIN/MAX', f'{func} @{pc:#x}', None, cnt / n, None, None,
                             MINMAX[mn].split('/'), f'{mn} over {nxt[1]}'))

    # ---- divisions by a constant power of two
    order = sorted(pcs)
    for i, pc in enumerate(order):
        cnt, mn, text, ln, func = pcs[pc]
        if mn not in ('quos', 'quou', 'rems', 'remu'):
            continue
        o = ops_of(text)
        divisor, const = o[2] if len(o) > 2 else None, None
        for back in order[max(0, i - 8):i][::-1]:
            bmn, btext = pcs[back][1], pcs[back][2]
            bo = ops_of(btext)
            if bo and bo[0] == divisor:
                if bmn in ('movi', 'movi.n'):
                    const = int(bo[1], 0)
                break
        pow2 = const is not None and const > 0 and const & (const - 1) == 0
        save = 15 * cnt / n if pow2 else 0
        label = (f'divisor {const} (power of two): SRAI/SRLI/EXTUI' if pow2 else
                 f'divisor {"in a register" if const is None else const}: not a shift')
        if save >= a.min_cycles:
            rows.append((save, 'DIV', f'{func} @{pc:#x}', None, cnt / n, None, None, ['SRAI', 'EXTUI'], label))
        else:
            rejected.append((16 * cnt / n, f'{func} @{pc:#x} ({mn})', None, cnt / n, label))

    rows.sort(key=lambda r: -r[0])
    print(f'turns {n}; model cycles/turn {total_cyc / n:,.0f} (no flash wait); savings are model estimates')
    print(f'\n== candidates (saved cycles/turn, % of the model turn)')
    print(f"{'#':>3} {'saved':>8} {'%':>6}  {'kind':12s} {'where':46s} {'trip':>6} {'entries':>8} {'B/entry':>8} {'now cyc':>8}  instructions (TRM page)  note")
    for i, (save, kind, where, trip, ent, bpe, now, insns, note) in enumerate(rows[:a.top], 1):
        ins = ', '.join(f'{x} p{db[x]}' if x in db else (x if not x.startswith('EE.') else f'{x} (not in TRM db)')
                        for x in insns)
        f = lambda v, fmt: format(v, fmt) if v is not None else '-'
        print(f'{i:3d} {save:8,.0f} {save / (total_cyc / n):6.2%}  {kind:12s} {where:46s} {f(trip, "6.1f")} '
              f'{f(ent, "8.1f")} {f(bpe, "8.0f")} {f(now, "8,.0f")}  {ins}  {note}')
    s = sum(r[0] for r in rows)
    print(f'    all {len(rows)} candidates together: {s:,.0f} cycles/turn ({s / (total_cyc / n):.1%}; '
          f'overlapping candidates are counted twice)')
    rejected.sort(key=lambda r: -r[0])
    print(f'\n== hottest rejected (cycles/turn spent there now, why not)')
    for now, where, trip, ent, why in rejected[:a.top]:
        f = lambda v, fmt: format(v, fmt) if v is not None else '-'
        print(f'    {now:8,.0f}  {where:50s} trip {f(trip, "6.1f")} entries {f(ent, "7.1f")}  {why}')


if __name__ == '__main__':
    main()
