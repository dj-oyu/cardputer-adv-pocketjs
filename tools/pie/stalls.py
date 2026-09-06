"""Static pipeline check for a PIE kernel: where will it interlock?

    python tools/pie/stalls.py main/shell.c ocean_row_pie
    python tools/pie/stalls.py main/render_accel.c blend_blocks_pie

Reads the inline assembly of the named function and applies TRM table 1.7-2:
loads, EE.LDXQ.32, EE.VMUL.*, EE.VRELU.* and EE.VPRELU.* define their QR result
at pipeline stage 2 (for the fused EE.*.LD.INCP forms: the loaded register at
stage 2, the arithmetic result at its own stage), everything else at stage 1,
and every instruction reads its QR operands at stage 1. Per TRM 1.7.1 a consumer must therefore be issued
D = def - use + 1 cycles after its producer: two cycles after a stage-2
producer. A consumer written *immediately* after such a producer costs one
stall cycle; one independent instruction in between costs nothing.

The report lists every such pair (including pairs that wrap from the end of
the loop body to its start), counts memory instructions, and estimates the
loop body size against the 256-byte limit of `loopgtz`. Measured on the
device, removing these pairs alone took the ocean kernel from 1.90 to 1.39
cycles per instruction; whatever remains after that is not a data stall.

Limitations: the QACC accumulator and SAR are not modelled (their def/use rows
in the table are incomplete), and the size estimate is from the usual encodings
(24-bit for EE ops, 32-bit for EE.LDXQ.32, narrow for mov/addi) — confirm with
objdump when close to the limit.
"""
import re
import sys
from piesim import extract_asm

# Producers that define their QR result at pipeline stage 2 (TRM table 1.7-2).
STAGE2 = {'ee.vld.128.ip', 'ee.vld.l.64.ip', 'ee.vldbc.16', 'ee.vldbc.16.ip', 'ee.ldxq.32',
          'ee.vmul.s16', 'ee.vmul.u16', 'ee.vrelu.s16', 'ee.vprelu.s16'}
MEMORY = {'ee.vld.128.ip', 'ee.vld.l.64.ip', 'ee.vldbc.16', 'ee.vldbc.16.ip', 'ee.ldxq.32',
          'ee.vst.128.ip'}
SIZE = {'ee.ldxq.32': 4, 'mov': 2, 'addi': 2, 'wsr.sar': 3, 'loopgtz': 3, 'bnez': 3}


def operands(op, qs):
    """-> (defs, uses): defs as (register, stage) pairs, uses as registers."""
    if op.endswith('.ld.incp'):                    # EE.<alu>.LD.INCP qu, as, qa, qx, qy
        base = op[:-len('.ld.incp')]
        return [(qs[0], 2), (qs[1], 2 if base in STAGE2 else 1)], qs[2:4]
    if op in ('ee.vst.128.ip', 'ee.vmulas.u16.qacc'):
        return [], qs
    if op in ('ee.vunzip.16', 'ee.vzip.8'):
        return [(q, 1) for q in qs], qs
    if op == 'ee.vrelu.s16':
        return [(qs[0], 2)], qs[:1]
    if op == 'ee.vprelu.s16':                      # qz, qx, qy, ay
        return [(qs[0], 2)], qs[1:3]
    if op in ('ee.zero.q', 'ee.srcmb.s16.qacc'):
        return [(qs[0], 1)], []
    if op in ('ee.vldbc.16', 'ee.vldbc.16.ip', 'ee.vld.128.ip', 'ee.vld.l.64.ip'):
        return [(qs[0], 2)], []
    if op == 'ee.ldxq.32':
        return [(qs[0], 2)], qs[1:2]
    if qs:
        return [(qs[0], 2 if op in STAGE2 else 1)], qs[1:]
    return [], []


def parse(asm):
    """-> list of (op, defs, uses) for the instructions of the main loop body:
    the longest range of a `loopgtz ... label:` pair or of a `label: ... bnez
    label` pair, or the whole text when there is no loop."""
    items = []                              # ('label', name) or ('ins', op, args, qregs)
    for raw in asm.splitlines():
        t = raw.split('/*')[0].strip()
        if not t:
            continue
        if re.fullmatch(r'[0-9A-Za-z_.]+:', t):
            items.append(('label', t[:-1]))
            continue
        op, *a = t.replace(',', ' ').split()
        items.append(('ins', op, a, [x for x in a if re.fullmatch(r'q[0-7]', x)]))
    ranges = []
    for i, it in enumerate(items):
        if it[0] == 'ins' and it[1] == 'loopgtz':
            lbl = it[2][1].rstrip('fb')
            end = next(j for j in range(i + 1, len(items)) if items[j] == ('label', lbl))
            ranges.append((i + 1, end))
        if it[0] == 'ins' and it[1] == 'bnez':
            lbl = it[2][1].rstrip('fb')
            start = next(j for j in range(i) if items[j] == ('label', lbl)) + 1
            ranges.append((start, i + 1))
    start, end = max(ranges, key=lambda r: r[1] - r[0]) if ranges else (0, len(items))
    body = []
    for it in items[start:end]:
        if it[0] == 'ins':
            defs, uses = operands(it[1], it[3])
            body.append((it[1], defs, uses))
    return body


def analyse(body):
    n = len(body)
    seq = body + body                      # second copy exposes wrap-around pairs
    stalls, seen = [], set()
    last_def = {}                          # reg -> (index, op, stage)
    for i, (op, defs, uses) in enumerate(seq):
        for r in uses:
            if r in last_def:
                j, pop, stage = last_def[r]
                if stage == 2 and i - j == 1:
                    key = (j % n, i % n)
                    if key not in seen:
                        seen.add(key)
                        stalls.append((j % n, pop, i % n, op, r))
        for r, stage in defs:
            last_def[r] = (i, op, stage)
    return stalls


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    path, func = sys.argv[1:]
    body = parse(extract_asm(path, func))
    stalls = analyse(body)
    total = len(body)
    mem = sum(op in MEMORY or op.endswith('.ld.incp') for op, _, _ in body)
    size = sum(SIZE.get(op, 3) for op, _, _ in body)
    print(f'{func}: {total} instructions per block, {mem} memory ({mem * 100 // max(total, 1)}%), '
          f'{sum(op == "ee.ldxq.32" for op, _, _ in body)} ldxq, '
          f'{sum(op.startswith("ee.vldbc") for op, _, _ in body)} vldbc')
    print(f'loop body ~{size} bytes' + ('  (> 256: loopgtz cannot be used)' if size > 256 else ''))
    if not stalls:
        print('no stage-2 producer followed immediately by its consumer: 0 data stalls')
    else:
        print(f'{len(stalls)} stall(s): producer -> consumer (index: op) on register')
        for j, pop, i, cop, r in stalls:
            print(f'  {j:3}: {pop:<20} -> {i:3}: {cop:<20} {r}')
    print(f'estimated issue cycles per block: {total + len(stalls)}')


if __name__ == '__main__':
    main()
