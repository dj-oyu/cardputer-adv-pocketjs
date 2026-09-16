#!/usr/bin/env python3
"""Placed bytes per archive member (and per name substring), from a link map.

    python3 member_bytes.py <map> [SUBSTR ...] [--total]

Without SUBSTR it prints the total placed bytes of the whole image.
With SUBSTR (e.g. compiler_builtins, cgu, libm) it prints one line per member whose
name contains it, sorted by bytes, plus the sum. Use it before/after a change to see
which member dropped.
"""
import sys
import collections

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from map_census import parse, SKIP, KEEP  # noqa: E402  (same parser, one source of truth)


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith('--')]
    path = argv[0]
    subs = argv[1:]
    place, _incl, _xref = parse(path)
    per = collections.Counter()
    for sym, e in place.items():
        for m, b in e['members'].items():
            per[m] += b
    total = sum(per.values())
    if not subs:
        print(f'{path}: placed total {total} B in {len(per)} members')
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
