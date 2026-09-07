"""Where a function's spill traffic is. NOT which values are spilling.

    python tools/pie/spills.py build_pie/cardputer_pocketjs.elf ray_row --all

**Read this first: a stack slot is not a value.** GCC allocates slots by live
range and puts several non-overlapping temporaries in the same one, so `a1+0
x68` means "the compiler wanted one scratch location sixty-eight times", not
"one value spilled sixty-eight times". Everything below reports traffic and
location. It is a better proxy than the store-to-arithmetic ratio and it is
still a proxy, and proxies have been expensive here (docs/pie-simd.md 3.8).

To learn which *variables* lost their registers you need more than the
disassembly. `-fdump-rtl-ira` says which pseudos were given memory
(`a12(r211,l0) -- assign memory`) but names them by pseudo number; tying those
back to source needs DWARF location lists. Until that is worth building, name a
slot by reading the loop's live set out of the source by hand and checking the
volume here against it. The two together are an answer; either alone is not.

Second limitation, and on inlined code it is fatal rather than annoying: loop
identification is a heuristic. Loops are found by looking for backward branches,
and a `continue`, a block GCC reordered, or a tail duplication all produce one
too. Run this on `ray_row` -- which has `bell_hit` inlined into it -- and it
reports fifteen overlapping "loops" around one square root, of which none is the
six-band loop in the source. It cannot find that loop, and neither can anyone
reading its output.

So the whole-function totals are the trustworthy part of what this prints, and
the per-loop attribution is only usable where the loops are real: a hand-written
kernel, or a function the compiler left alone. For anything inlined, take the
totals from here and the live set from the source.

---

`stalls.py` answers "is this instruction sequence fast"; this answers "does this
function have more live values than registers". They are different failures and
the second has been the more expensive one in this codebase: `shade` lost 39% of
its time for a 14% cut in instructions, and a rewrite of `bell_hit`'s setup that
removed twelve arithmetic operations was worth 417 cycles a visit -- both because
the saving was in the memory traffic around the work, not in the work.

Two kinds of traffic, wanting opposite fixes:

  carried   a slot the loop only ever reads, written before the loop. A value
            the compiler could not keep in a register across the body. Fix by
            shortening the live range -- recompute it, or restructure so it is
            not live across the whole loop.

  churned   a slot written and read inside the body. A temporary that did not
            fit. Fix by splitting the computation so fewer values coexist, or by
            hoisting some of them out of the loop entirely.
"""
import re
import subprocess
import sys
import collections
import os

# Xtensa float and integer stack traffic. The float forms are what matter here;
# the integer ones are reported separately because an integer spill in a float
# loop usually means a pointer or an index, which is a different problem.
FLOAT_LD = ('lsi', 'lsip', 'lsx', 'lsxp')
FLOAT_ST = ('ssi', 'ssip', 'ssx', 'ssxp')
INT_LD = ('l32i', 'l32i.n')
INT_ST = ('s32i', 's32i.n')
FP_ALU = re.compile(r'\.s$')


def disassemble(path, func):
    """Return the instruction lines of `func`. Accepts an ELF or a text dump."""
    if path.endswith('.dis') or path.endswith('.txt'):
        text = open(path, errors='ignore').read()
    else:
        objdump = os.environ.get('OBJDUMP')
        if not objdump:
            for base in ('xtensa-esp32s3-elf-objdump', 'xtensa-esp-elf-objdump'):
                for root, _, files in os.walk(r'C:\Espressif\tools\xtensa-esp-elf'):
                    for f in files:
                        if f.startswith(base):
                            objdump = os.path.join(root, f)
                            break
                    if objdump:
                        break
                if objdump:
                    break
        if not objdump:
            sys.exit('set OBJDUMP to an xtensa objdump, or pass a .dis file')
        text = subprocess.run([objdump, '-d', path], capture_output=True,
                              text=True, errors='ignore').stdout
    m = re.search(r'<%s(?:\.[a-z0-9_.]+)?>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)' % re.escape(func),
                  text, re.S)
    if not m:
        sys.exit('no function <%s> in that image' % func)
    out = []
    for line in m.group(1).splitlines():
        mm = re.match(r'\s*([0-9a-f]+):\t\S+\s*\t(.*)', line)
        if mm:
            body = mm.group(2).split('/*')[0].strip()
            if body:
                out.append((int(mm.group(1), 16), body.split()[0],
                            body[len(body.split()[0]):].strip()))
    return out


def loops(insns):
    """Every backward branch, as (start_index, end_index, bytes). Innermost
    first, so a nested pair is reported inside out."""
    addr = [a for a, _, _ in insns]
    found = []
    for i, (a, op, args) in enumerate(insns):
        if not (op.startswith('b') or op in ('loop', 'loopgtz', 'loopnez', 'j')):
            continue
        t = re.search(r'\b([0-9a-f]{4,})\b', args)
        if not t:
            continue
        target = int(t.group(1), 16)
        if target >= a:                       # forward: a loop header, not a back edge
            if op.startswith('loop'):
                j = next((k for k, x in enumerate(addr) if x >= target), None)
                if j is not None and j > i:
                    found.append((i + 1, j - 1, target - addr[i + 1]))
            continue
        j = next((k for k, x in enumerate(addr) if x >= target), None)
        if j is not None and j <= i:
            found.append((j, i, a - target))
    found.sort(key=lambda t: t[2])
    return found


def slot(args):
    """`f3, a1, 40` -> ('a1', 40). None when the base is not a frame pointer."""
    m = re.match(r'\w+,\s*(a\d+),\s*(-?\d+)', args)
    return (m.group(1), int(m.group(2))) if m else None


def report(insns, lo, hi, span, verbose):
    body = insns[lo:hi + 1]
    fp_alu = sum(1 for _, op, _ in body if FP_ALU.search(op))
    reads, writes = collections.Counter(), collections.Counter()
    ireads, iwrites = collections.Counter(), collections.Counter()
    for _, op, args in body:
        s = slot(args)
        if not s:
            continue
        if op in FLOAT_LD:
            reads[s] += 1
        elif op in FLOAT_ST:
            writes[s] += 1
        elif op in INT_LD:
            ireads[s] += 1
        elif op in INT_ST:
            iwrites[s] += 1
    before = collections.Counter()
    for _, op, args in insns[:lo]:
        s = slot(args)
        if s and op in FLOAT_ST:
            before[s] += 1

    print('loop at %d..%d, %d instructions, %d bytes, %d float ALU'
          % (lo, hi, len(body), span, fp_alu))
    fl = sum(reads.values()) + sum(writes.values())
    print('  float stack traffic %d (%d loads, %d stores) = %.2f per float ALU op'
          % (fl, sum(reads.values()), sum(writes.values()),
             fl / fp_alu if fp_alu else 0))
    if not fl:
        print('  nothing spilling here')
        return
    carried = [(s, n) for s, n in reads.items() if s not in writes and s in before]
    churned = [(s, reads[s] + writes[s]) for s in writes]
    other = [(s, n) for s, n in reads.items() if s not in writes and s not in before]
    for name, items in (('carried', carried), ('churned', churned), ('read-only', other)):
        if not items:
            continue
        items.sort(key=lambda t: -t[1])
        print('  %s:' % name, ', '.join('%s%+d x%d' % (s[0], s[1], n)
                                        for s, n in (items if verbose else items[:8])))
    ii = sum(ireads.values()) + sum(iwrites.values())
    if ii:
        print('  integer stack traffic %d (pointers and indices, usually a different problem)' % ii)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    verbose = '--all' in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    insns = disassemble(args[0], args[1])
    print('%s: %d instructions' % (args[1], len(insns)))
    ls = loops(insns)
    if not ls:
        print('no loops found; reporting the whole function')
        report(insns, 0, len(insns) - 1, 0, verbose)
        return
    if not verbose:
        # `ls` is sorted innermost first. Per-pixel cost lives in the innermost
        # loop that touches the stack at all; the larger ones are per-row and
        # per-part scaffolding and their totals are dominated by the inner body
        # they contain, which double-counts it.
        def traffic(t):
            return sum(1 for _, op, a in insns[t[0]:t[1] + 1]
                       if (op in FLOAT_LD or op in FLOAT_ST) and slot(a))
        inner = [t for t in ls if traffic(t)]
        ls = [inner[0]] if inner else [ls[0]]
    for lo, hi, span in ls:
        report(insns, lo, hi, span, verbose)
        print()


if __name__ == '__main__':
    main()
