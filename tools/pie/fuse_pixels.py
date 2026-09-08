"""Fuse the pixel pass's constant loads into the arithmetic that precedes them.

garden_pixels_pie walks forty broadcast constants with plain EE.VLD.128.IP.
EE.VADDS/VSUBS/VMUL have a .LD.INCP form that does the same load in the same
issue slot for free (docs/pie-simd.md 3.4), so every load that can be given a
host is an instruction that stops existing.

This is a generator rather than a hand edit because three things have to hold at
once and none of them is locally visible:

  * the constant walk IS the program order of the loads. A load that moves past
    another swaps two constants -- code that assembles, runs, and comes out the
    wrong colour somewhere.
  * the loaded register must be dead from the host to the load's old position,
    and the host's own sources are read before the load, so a load into one of
    them is legal and a load into the destination is not.
  * fusing removes an instruction, and the instruction it removes may have been
    the only thing standing between a stage-2 producer and its consumer. A
    fusion that saves a slot and buys a stall is worth nothing.

So every candidate is checked against the pipeline model in stalls.py, in BOTH
spellings -- the fused kernel and the control build that GARDEN_PIE_FUSE=0
selects -- and rejected if either gains a stall. The result is written back into
the asm block as P_ADD/P_SUB/P_MUL macros, which piesim.expand_fuse expands
either way so the tools can read both.

    python tools/pie/fuse_pixels.py            # report
    python tools/pie/fuse_pixels.py --write    # rewrite the asm block
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
from stalls import operands

SRC = 'main/scene/garden.c'
AR = re.compile(r'^ee\.(vadds\.s16|vsubs\.s16|vmul\.s16)\s+(q\d), (q\d), (q\d)$')
KPLD = re.compile(r'^ee\.vld\.128\.ip\s+(q\d), %\[kp\], 16$')
MAC = {'vadds.s16': 'P_ADD', 'vsubs.s16': 'P_SUB', 'vmul.s16': 'P_MUL'}


def asm_of(line):
    m = re.search('"\s*(.*?)' + re.escape(chr(92) + 'n') + '"', line)
    return m.group(1).strip() if m else ''


def comment_of(line):
    m = re.search(r'/\*(.*?)\*/', line)
    return m.group(1).strip() if m else ''


def rw(a):
    op = a.split()[0]
    qs = ['q' + x for x in re.findall(r'\bq(\d)\b', a)]
    defs, uses = operands(op, qs)
    return defs, set(uses)


def stalls(body):
    """Adjacent stage-2 producer feeding its consumer, the loop wrap included."""
    n, bad = len(body), 0
    for i in range(n):
        defs, _ = rw(body[i])
        _, uses = rw(body[(i + 1) % n])
        if any(st == 2 and r in uses for r, st in defs):
            bad += 1
    return bad


def fused_text(op, z, x, y, ld):
    return 'ee.%s.ld.incp %s, %%[kp], %s, %s, %s' % (op, ld, z, x, y)


def plain_pair(op, z, x, y, ld):
    """The control spelling, and it must be the one piesim.expand_fuse and the
    C macro produce or the thing being checked is not the thing being built.

    Always arithmetic first, then the load. The fused instruction reads its
    sources before it loads, so a load into one of its own sources is legal --
    and putting the load after is the only ordering that reproduces that. It is
    right for the other case too, so there is one rule instead of two."""
    return ['ee.%s %s, %s, %s' % (op, z, x, y), 'ee.vld.128.ip %s, %%[kp], 16' % ld]


def main():
    lines = open(SRC, encoding='utf-8').read().split('\n')
    top = [i for i, l in enumerate(lines) if 'garden_pixels_pie(uint16_t' in l][0]
    lab = [i for i in range(top, len(lines)) if lines[i].strip() == '"1:' + chr(92) + 'n"'][0]
    end = [i for i in range(lab, len(lines)) if 'bnez' in lines[i]][0]
    # Idempotent: if the block is already fused, unfuse it first. A generator
    # that only runs on virgin input is one nobody can re-run after editing the
    # arithmetic, which is the case it exists for.
    raw = []
    for t in lines[lab + 1:end + 1]:
        m = re.match(r'\s*P_(ADD|SUB|MUL)\("(q\d)","(q\d)","(q\d)","(q\d)"\)', t)
        if not m:
            raw.append(t)
            continue
        op = {'ADD': 'vadds.s16', 'SUB': 'vsubs.s16', 'MUL': 'vmul.s16'}[m.group(1)]
        z, x, y, ld = m.group(2), m.group(3), m.group(4), m.group(5)
        note = comment_of(t)
        head, _, tail = note.partition('; load ')
        raw.append('        "  ee.%s %s, %s, %s%s"%s' % (op, z, x, y, chr(92) + 'n',
                   ('' if not head else ' ' * 12 + '/* %s */' % head)))
        raw.append('        "  ee.vld.128.ip        %s, %%[kp], 16%s"%s' % (ld, chr(92) + 'n',
                   ('' if not tail else ' ' * 12 + '/* %s */' % tail)))
    body = [asm_of(t) for t in raw]
    n = len(body)
    loads = [i for i in range(n) if KPLD.match(body[i])]
    hosts = [i for i in range(n) if AR.match(body[i])]
    base_stalls = stalls(body)

    assign, taken, prev = {}, set(), -1
    for idx, li in enumerate(loads):
        ldr = KPLD.match(body[li]).group(1)
        upper = loads[idx + 1] if idx + 1 < len(loads) else n
        chosen = None
        for j in hosts:
            if j in taken or j <= prev or j >= upper:
                continue
            m = AR.match(body[j])
            op, z, x, y = m.group(1), m.group(2), m.group(3), m.group(4)
            if z == ldr:
                continue                        # the load would clobber the result
            lo, hi = min(j, li), max(j, li)
            dead = True
            for k in range(lo, hi + 1):
                if k == li or k == j:
                    continue
                d, u = rw(body[k])
                if ldr in u or any(r == ldr for r, _ in d):
                    dead = False
                    break
            if not dead:
                continue
            if j > li:                          # moving later: the old value must not be read
                if any(ldr in rw(body[k])[1] for k in range(li + 1, j + 1)):
                    continue
            trial = list(body)
            trial[j] = fused_text(op, z, x, y, ldr)
            ctrl = list(body)
            ctrl[j] = plain_pair(op, z, x, y, ldr)
            del trial[li]
            ctrl[li] = None
            flat = []
            for e in ctrl:
                if e is None:
                    continue
                flat.extend(e if isinstance(e, list) else [e])
            if stalls(trial) > base_stalls or stalls(flat) > base_stalls:
                continue
            chosen = j
            break
        if chosen is None:
            prev = li
            continue
        m = AR.match(body[chosen])
        body[chosen] = fused_text(m.group(1), m.group(2), m.group(3), m.group(4), ldr)
        assign[li] = chosen
        taken.add(chosen)
        prev = chosen

    kept = [i for i in range(n) if i not in assign]
    print('%d instructions, %d constants, %d fused -> %d instructions'
          % (n, len(loads), len(assign), len(kept)))
    print('stalls: %d before, %d fused, control checked per candidate' % (base_stalls, stalls([body[i] for i in kept])))
    if '--write' not in sys.argv:
        return
    inv = {v: k for k, v in assign.items()}
    out = []
    for i in kept:
        if i in inv:
            li = inv[i]
            ldr = KPLD.match(asm_of(raw[li])).group(1)
            m = AR.match(asm_of(raw[i]))
            note = '; '.join(x for x in (comment_of(raw[i]),
                                         'load ' + comment_of(raw[li]) if comment_of(raw[li]) else '') if x)
            s = '        %s("%s","%s","%s","%s")' % (MAC[m.group(1)], m.group(2), m.group(3), m.group(4), ldr)
            if note:
                s += ' ' * max(1, 52 - len(s)) + '/* %s */' % note
            out.append(s)
        else:
            out.append(raw[i])
    lines[lab + 1:end + 1] = out
    open(SRC, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    print('written')


if __name__ == '__main__':
    main()
