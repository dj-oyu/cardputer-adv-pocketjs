"""Instructions and taken control transfers per turn, from an icache.py pickle.

    python tools/r5sim/jumps.py turns.pkl.gz build/r5sim.elf --objdump <objdump>

A layout change (docs/vm/qjs-hotcold.md) moves no work, but it changes which
branches are taken: a block that used to be skipped by a taken branch may now
be fallen past, and a block moved away is reached by a jump. On this core a
taken transfer costs about two cycles more than a fall-through (CLAUDE.md:
cycles = instructions + 2 x taken + flash waits), so the model's instruction
count alone would miss it.

A transfer is "taken" when the next executed block does not start where the
previous one ended. Only flash code is in the trace, so a call into IRAM or
ROM and its return count as one taken transfer, as they would if seen.
"""
import argparse, gzip, os, pickle, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lineuse import insn_lengths, guess_len  # noqa: E402

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('turns'); ap.add_argument('elf')
    ap.add_argument('--objdump', default='xtensa-esp32s3-elf-objdump')
    a = ap.parse_args()
    d = pickle.load(gzip.open(a.turns, 'rb'))
    lens = insn_lengths(a.objdump, a.elf)
    first, after, count = [], [], []
    for addrs in d['blocks']:
        last = addrs[-1]
        first.append(addrs[0])
        after.append(last + (lens.get(last) or guess_len(last, None)))
        count.append(len(addrs))
    n = len(d['turns'])
    insns = taken = blocks = 0
    for t in d['turns']:
        prev = None
        for b in t:
            insns += count[b]
            if prev is not None and first[b] != prev:
                taken += 1
            prev = after[b]
        blocks += len(t)
    print(f'{a.elf}: {n} turns; per turn: {insns / n:.0f} instructions in flash, {blocks / n:.0f} blocks, '
          f'{taken / n:.0f} taken transfers; instructions + 2 x taken = {(insns + 2 * taken) / n:.0f}')

if __name__ == '__main__':
    main()
