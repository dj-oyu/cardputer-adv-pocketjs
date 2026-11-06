"""Whole-turn observation of a QEMU trace of r5sim (docs/vm/qjs-observe.md).

Unlike icache.py, which keeps only the flash fetches, this reads every
executed instruction of the measured turns -- ROM (libgcc's soft double and
64-bit integer helpers live there), IRAM (window-exception vectors, ISRs,
IRAM helpers) and flash -- and reports, per turn:

  1. instructions per function (exclusive), grouped into parts;
  2. calls into libgcc's 64-bit integer and soft double/float helpers, the
     instructions spent in them and who called them;
  3. a static pipeline model on the dynamic stream: taken control transfers
     (2 bubbles each, measured on this board: CLAUDE.md, docs/perf/pie-simd.md
     sec.3.9), load-use pairs (1 cycle, measured: esp32s3-hw measured_timing
     native_load_use), L32R, window over/underflow exceptions, MUL/DIV;
  4. the hottest loops (a taken backward branch inside one function), with
     trip counts and body sizes: where a PIE/zero-overhead loop could go.

QEMU is not cycle accurate and models no cache; every cycle figure printed
here is the model above applied to the instruction stream, an estimate.

    qemu-system-xtensa ... -icount shift=2 -d exec,nochain,in_asm \
        -dfilter 0x40000000..0x403fffff,0x42000000..0x43ffffff -D r5.log
    python tools/r5sim/observe.py r5.log <dir>/r5sim.elf --nm <nm> \
        [--first 61] [--last 120] [--json out.json]
"""
import argparse, bisect, collections, json, re, subprocess, sys

LOADS = {'l8ui', 'l16ui', 'l16si', 'l32i', 'l32i.n', 'l32e', 'l32ai', 'lsi', 'lsip', 'lsx', 'lsxp', 'l32r'}
STORES = {'s8i', 's16i', 's32i', 's32i.n', 's32e', 's32ri', 'ssi', 'ssip', 'ssx', 'ssxp'}
NODST = STORES | {'j', 'jx', 'call0', 'call4', 'call8', 'call12', 'callx0', 'callx4', 'callx8', 'callx12',
                  'ret', 'ret.n', 'retw', 'retw.n', 'entry', 'loop', 'loopnez', 'loopgtz', 'wsr', 'wur',
                  'wfr', 'memw', 'isync', 'rsync', 'esync', 'dsync', 'nop', 'nop.n', 'ill', 'break',
                  'wer', 'witlb', 'wdtlb', 'ssl', 'ssr', 'ssa8l', 'ssa8b', 'ssai', 'rfe', 'rfwo', 'rfwu',
                  'rfi', 'excw', 'extw', 'waiti', 's32c1i'}
CALLS = {'call0', 'call4', 'call8', 'call12', 'callx0', 'callx4', 'callx8', 'callx12'}
RETS = {'ret', 'ret.n', 'retw', 'retw.n', 'rfe', 'rfwo', 'rfwu', 'rfi'}
JUMPS = {'j', 'jx'}
MULDIV = {'mull', 'muluh', 'mulsh', 'mul16u', 'mul16s', 'quos', 'quou', 'rems', 'remu'}
DIVS = {'quos', 'quou', 'rems', 'remu'}
REG = re.compile(r'\b([af]\d{1,2}|b\d{1,2})\b')

# libgcc helpers: the "precision" question. 64-bit integer, soft double,
# soft float (the S3 FPU does single precision in hardware; libgcc's *sf3 for
# an op the FPU has should not appear), conversions and compares.
HELPERS = re.compile(r'^__(?:(?:u?(?:div|mod)di3|muldi3|ashldi3|ashrdi3|lshrdi3|(?:u?cmp|neg)di2|'
                     r'(?:add|sub|mul|div|neg)[sd]f3|(?:eq|ne|lt|le|gt|ge|un)[sd]f2|'
                     r'fix(?:uns)?[sd]f[sd]i|float(?:un)?[sd]i[sd]f|extendsfdf2|truncdfsf2|'
                     r'(?:ctz|clz|popcount|ffs)[sd]i2|bswap[sd]i2))$')


def parse_insn(text):
    """'l32i.n\ta3, a2, 4' -> (mnem, dsts, srcs, is_load)."""
    mn, _, ops = text.partition('\t')
    mn = mn.strip()
    regs = REG.findall(ops)
    if mn.startswith('b') and mn not in ('break',) or mn in NODST:
        return mn, (), tuple(regs), False
    if mn in ('s32c1i',):
        return mn, (regs[0],) if regs else (), tuple(regs), True
    if not regs:
        return mn, (), (), mn in LOADS
    return mn, (regs[0],), tuple(regs[1:]), mn in LOADS


def symbols(nm, elf, rom_elf=None):
    """Sorted (addr, name). The image's own ELF only knows the ROM through
    the linker's PROVIDE lines, which name the jump-table stubs
    (__call___adddf3 at 0x40002184) and a handful of entry points: every ROM
    body would be charged to whichever PROVIDE precedes it. The ROM ELF
    (esp-rom-elfs) names the bodies, so ROM addresses come from it."""
    def read(path, keep):
        out = subprocess.run([nm, '-n', '--defined-only', path], capture_output=True, text=True).stdout
        for line in out.splitlines():
            p = line.split()
            if len(p) == 3 and p[1] in 'tTwWA' and keep(int(p[0], 16)):
                yield int(p[0], 16), p[2]
    rom = lambda a: 0x40000000 <= a < 0x40060000
    syms = list(read(elf, lambda a: (0x40000000 <= a < 0x40400000 and not (rom_elf and rom(a)))
                     or 0x42000000 <= a < 0x44000000))
    if rom_elf:
        syms += list(read(rom_elf, rom))
    syms.sort()
    addrs, names = [], []
    for a, n in syms:
        if n.startswith('__call___'):
            n = n[len('__call_'):]      # the stub counts as the helper itself
        if addrs and addrs[-1] == a:
            continue  # aliases: keep the first name
        addrs.append(a); names.append(n)
    return addrs, names


def region(a):
    if a < 0x40060000:
        return 'rom'
    if a < 0x40400000:
        return 'iram'
    return 'flash'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log'); ap.add_argument('elf')
    ap.add_argument('--nm', default='xtensa-esp32s3-elf-nm')
    ap.add_argument('--rom-elf', help='esp-rom-elfs esp32s3_rev0_rom.elf')
    ap.add_argument('--first', type=int, default=61)
    ap.add_argument('--last', type=int, default=120)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--json')
    a = ap.parse_args()

    saddr, sname = symbols(a.nm, a.elf, a.rom_elf)
    begin_pc = saddr[sname.index('r5_turn_begin')]
    end_pc = saddr[sname.index('r5_turn_end')]
    func_cache = {}

    def func_of(pc):
        f = func_cache.get(pc)
        if f is None:
            i = bisect.bisect_right(saddr, pc) - 1
            f = sname[i] if i >= 0 else '?'
            func_cache[pc] = f
        return f

    # TB table keyed by host pointer (stable while the TB lives; a new IN:
    # block followed by its Trace line rebinds it).
    tbs = {}
    pending = None          # insns of the IN: block just read
    started = False; turn = 0; begins = 0
    # per-turn-window accumulators
    insn_by_func = collections.Counter()
    taken_by_func = collections.Counter()
    loaduse_by_func = collections.Counter()
    l32r_by_func = collections.Counter()
    mnem = collections.Counter()
    xfer = collections.Counter()          # kind of control transfer at TB ends
    helper_calls = collections.Counter(); helper_callers = collections.Counter()
    loops = collections.Counter()          # (func, branch pc) -> taken backward
    loop_body = {}
    region_insn = collections.Counter()
    exc_entries = collections.Counter()
    edges = collections.Counter()
    prev = None   # (tb, func_at_end)

    def summarize(insns):
        """Static per-TB facts: list of (pc, len, mn, dsts, srcs, load)."""
        n = len(insns)
        lu = 0; lu_at = []
        for i in range(n - 1):
            pc, ln, mn, d, s, ld = insns[i]
            if ld and d and d[0] in insns[i + 1][4]:
                lu += 1
        return lu

    with open(a.log, 'rb') as f:
        in_block = False
        for raw in f:
            c = raw[:1]
            if c == b'T':   # Trace 0: 0xHOST [xxxxxxxx/PC/flags/..]
                sp = raw.split(b'[', 1)[1].split(b'/', 2)
                pc = int(sp[1], 16)
                host = raw[9:raw.index(b' ', 9)]
                if pending is not None:
                    ins = pending; pending = None
                    tb = (pc, ins, summarize(ins), func_of(ins[-1][0]) if ins else '?')
                    tbs[host] = tb
                else:
                    tb = tbs.get(host)
                    if tb is None or tb[0] != pc:
                        prev = None
                        continue
                if pc == begin_pc:
                    begins += 1
                    if begins == a.first:
                        started = True
                if not started:
                    prev = tb
                    continue
                ins = tb[1]
                # transfer at the end of the previous TB
                if prev is not None:
                    ppc, pins, _, pfunc = prev
                    last = pins[-1]
                    fall = last[0] + last[1]
                    lmn = last[2]
                    if pc == fall:
                        k = 'fall_branch' if (lmn.startswith('b') and lmn != 'break') else 'fall_split'
                    elif lmn in CALLS: k = 'call'
                    elif lmn in RETS: k = 'ret'
                    elif lmn in JUMPS: k = 'jump' if lmn == 'j' else 'jx'
                    elif lmn.startswith('b') and lmn != 'break':
                        k = 'branch_taken'
                        if pc <= last[0] and func_of(pc) == pfunc:
                            loops[(pfunc, last[0])] += 1
                            loop_body.setdefault((pfunc, last[0]), (pc, last[0] + last[1] - pc))
                    else:
                        k = 'other'   # exception/interrupt entry, zero-overhead loop back
                    xfer[k] += 1
                    if k in ('call', 'ret', 'jump', 'jx', 'branch_taken'):
                        taken_by_func[pfunc] += 1
                    if k == 'other' and region(pc) == 'iram':
                        exc_entries[func_of(pc)] += 1
                    # load-use across the TB boundary
                    if last[5] and last[3] and ins and last[3][0] in ins[0][4]:
                        loaduse_by_func[pfunc] += 1
                fn0 = func_of(pc)
                if prev is not None and fn0 != prev[3] and prev[1][-1][2] in CALLS:
                    edges[(prev[3], fn0)] += 1      # caller -> callee, by call insn
                if (pc == saddr[bisect.bisect_right(saddr, pc) - 1] and HELPERS.match(fn0)
                        and not (prev and prev[3] == fn0)):
                    # one call = the stub's entry, or the body's entry when
                    # it is reached directly; the stub-to-body jump is not a
                    # second call (prev ends in the same-named stub)
                    helper_calls[fn0] += 1
                    helper_callers[(fn0, prev[3] if prev else '?')] += 1
                # this TB's instructions
                for (ipc, ln, mn, d, s, ld) in ins:
                    fn = func_of(ipc)
                    insn_by_func[fn] += 1
                    mnem[mn] += 1
                    region_insn[region(ipc)] += 1
                    if mn == 'l32r':
                        l32r_by_func[fn] += 1
                if tb[2]:
                    loaduse_by_func[func_of(ins[-1][0])] += tb[2]
                prev = tb
                if pc == end_pc:
                    turn += 1
                    if begins >= a.last:
                        break
            elif c == b'I' and raw.startswith(b'IN:'):
                pending = []
                in_block = True
            elif c == b'0' and in_block and raw.startswith(b'0x'):
                s = raw.decode('ascii', 'replace').rstrip('\n')
                addr_s, _, text = s.partition(':')
                text = text.strip()
                pc = int(addr_s, 16)
                mn, d, sr, ld = parse_insn(text)
                pending.append([pc, 0, mn, d, sr, ld])
            elif c == b'\n' and in_block:
                in_block = False
                if pending:
                    for i in range(len(pending) - 1):
                        pending[i][1] = pending[i + 1][0] - pending[i][0]
                    lm = pending[-1][2]
                    pending[-1][1] = 2 if lm.endswith('.n') else 3
                    pending = [tuple(x) for x in pending]

    nturn = max(turn, 1)
    total = sum(insn_by_func.values())
    taken = sum(taken_by_func.values())
    lu = sum(loaduse_by_func.values())
    l32r = sum(l32r_by_func.values())
    divs = sum(mnem[m] for m in DIVS)
    win = sum(v for k, v in exc_entries.items() if 'Window' in k)
    print(f'turns {nturn} ({a.first}..{a.last}); per turn below')
    print(f'insns {total/nturn:,.0f}  by region ' +
          ', '.join(f'{k} {v/nturn:,.0f}' for k, v in region_insn.most_common()))
    print('transfers: ' + ', '.join(f'{k} {v/nturn:,.0f}' for k, v in xfer.most_common()))
    model = total + 2 * taken + lu + 16 * divs
    print(f'model cycles (no cache): insns {total/nturn:,.0f} + 2x taken {taken/nturn:,.0f} '
          f'+ load-use {lu/nturn:,.0f} + div~16 x {divs/nturn:,.0f} = {model/nturn:,.0f} '
          f'(IPC {total/model:.3f})')
    print(f'l32r {l32r/nturn:,.0f}  window/exception vector entries ' +
          ', '.join(f'{k} {v/nturn:,.1f}' for k, v in exc_entries.most_common(8)))
    print('\n== mul/div/fpu mnemonics per turn')
    for m, v in sorted(mnem.items(), key=lambda x: -x[1]):
        if m in MULDIV or m.endswith('.s') or m in ('loop', 'loopnez', 'loopgtz'):
            print(f'  {m:10s} {v/nturn:10,.1f}')
    print('\n== libgcc helpers (entries per turn, insns per turn inside)')
    for h, v in helper_calls.most_common():
        print(f'  {h:16s} calls {v/nturn:9,.1f}  insns {insn_by_func[h]/nturn:10,.0f}  '
              f'({insn_by_func[h]/max(v,1):.0f}/call)')
        for (hh, caller), w in helper_callers.most_common():
            if hh == h and w / v >= 0.03:
                print(f'      from {caller:40s} {w/nturn:9,.1f}')
    hel_ins = sum(insn_by_func[h] for h in helper_calls)
    print(f'  helpers total insns/turn {hel_ins/nturn:,.0f} ({hel_ins/total:.1%} of insns)')
    print(f'\n== top functions (insns/turn, taken/turn, load-use/turn, l32r/turn, model cyc share)')
    rows = []
    for fn, v in insn_by_func.items():
        cyc = v + 2 * taken_by_func[fn] + loaduse_by_func[fn]
        rows.append((cyc, fn, v))
    rows.sort(reverse=True)
    for cyc, fn, v in rows[:a.top]:
        print(f'  {fn:44s} {v/nturn:9,.0f} {taken_by_func[fn]/nturn:8,.0f} '
              f'{loaduse_by_func[fn]/nturn:7,.0f} {l32r_by_func[fn]/nturn:7,.0f}  {cyc/model:6.2%}')
    print('\n== hottest loops (backward taken branches): func, branch pc, iterations/turn, body bytes')
    for (fn, bpc), v in loops.most_common(a.top):
        st, size = loop_body[(fn, bpc)]
        print(f'  {fn:40s} {bpc:#x} {v/nturn:9,.0f}  body {size} B @ {st:#x}')
    if a.json:
        json.dump({'turns': nturn, 'insn_by_func': insn_by_func, 'taken_by_func': taken_by_func,
                   'loaduse_by_func': loaduse_by_func, 'l32r_by_func': l32r_by_func,
                   'mnem': mnem, 'xfer': xfer, 'helper_calls': helper_calls,
                   'helper_callers': {f'{k[0]}<-{k[1]}': v for k, v in helper_callers.items()},
                   'loops': {f'{k[0]}@{k[1]:#x}': v for k, v in loops.items()},
                   'exc_entries': exc_entries, 'region_insn': region_insn,
                   'edges': {f'{k[0]}->{k[1]}': v for k, v in edges.items()}}, open(a.json, 'w'), indent=1)


if __name__ == '__main__':
    main()
