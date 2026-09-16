#!/usr/bin/env python3
"""Placed bytes per archive member (and per name substring), from a link map.

    python3 member_bytes.py <map> [SUBSTR ...] [--total] [--droppable [MIN_BYTES]]

Without SUBSTR it prints the total placed bytes of the whole image.
With SUBSTR (e.g. compiler_builtins, cgu, libm) it prints one line per member whose
name contains it, sorted by bytes, plus the sum. Use it before/after a change to see
which member dropped.

--droppable answers the size question properly: a member can be dropped only if
EVERY placed symbol in it is referenced by our own objects alone (a single foreign
referrer keeps the whole member). It prints two lists - the droppable members by
bytes, and the ones that only look droppable because the map's "Archive member
included to satisfy reference by ..." line names the FIRST referrer, not the only one.
"""
import sys
import collections

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from map_census import parse, kind  # noqa: E402  (same parser, one source of truth)

ENGINE = ('libquickjs', 'libpocketjs', 'compiler_builtins', 'libopus', 'libminimp3')
OURS_OBJ = ('libmain.a(',)


def member_bytes(place):
    per = collections.Counter()
    syms = collections.defaultdict(list)
    for sym, e in place.items():
        for m, b in e['members'].items():
            per[m] += b
            syms[m].append((b, sym))
    return per, syms


def droppable(place, xref, min_bytes, keep_engine=False):
    per, syms = member_bytes(place)
    yes, no = [], []
    for m, ms in syms.items():
        short = m.split('/')[-1]
        if any(e in short for e in ENGINE) and not keep_engine:
            continue
        if any(o in short for o in OURS_OBJ) and '--keep-ours' not in sys.argv:
            continue
        tot = per[m]
        if tot < min_bytes:
            continue
        owe, foreign = [], []
        for b, s in ms:
            refs = xref.get(s)
            if not refs:
                continue                      # data/asm symbol: no xref entry to judge
            ks = {kind(r) for r in refs}
            if ks - {'ours'}:
                foreign.append((s, sorted(ks - {'ours'})))
            elif 'ours' in ks:
                owe.append((b, s))
        (no if foreign else yes).append((tot, short, owe, foreign, len(ms)))
    return yes, no


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith('--')]
    flags = [a for a in sys.argv[1:] if a.startswith('--')]
    path = argv[0]
    subs = argv[1:]
    place, _incl, xref = parse(path)

    if '--droppable' in ' '.join(flags) and '--droppable' in sys.argv:
        i = sys.argv.index('--droppable')
        min_bytes = int(sys.argv[i + 1]) if len(sys.argv) > i + 1 and sys.argv[i + 1].isdigit() else 200
        yes, no = droppable(place, xref, min_bytes, keep_engine='--keep-engine' in sys.argv)
        print(f'== droppable by removing OUR references alone (>= {min_bytes} B) ==')
        for tot, short, owe, _f, n in sorted(yes, reverse=True):
            entry = ', '.join(s for _b, s in sorted(owe, reverse=True)[:3]) or '(データ/未判定)'
            print(f'{tot:>7} B  {short[:52]:<52} 入口: {entry[:56]}')
        print(f'  合計 {sum(t for t, *_ in yes)} B / {len(yes)} 会員')
        print(f'\n== 見た目は我々だけでも他の参照元が同じ会員を押さえている（落ちない） ==')
        for tot, short, owe, foreign, n in sorted(no, reverse=True)[:15]:
            f = ', '.join(f'{s}({",".join(k)})' for s, k in foreign[:2])
            print(f'{tot:>7} B  {short[:46]:<46} 他人: {f[:56]}')
        return

    per, _syms = member_bytes(place)
    if not subs:
        print(f'{path}: placed total {sum(per.values())} B in {len(per)} members')
        return
    for sub in subs:
        rows = [(b, m) for m, b in per.items() if sub in m]
        short = collections.Counter()
        for b, m in rows:
            short[m.split('(')[-1].rstrip(')')] += b
        print(f'== {sub}: {sum(b for b, _ in rows)} B in {len(rows)} members ==')
        for m, b in short.most_common(12):
            print(f'{b:>7}  {m[:104]}')


main()
