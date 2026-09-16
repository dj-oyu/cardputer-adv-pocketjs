#!/usr/bin/env python3
"""Census of an ESP-IDF link map: what is placed, who keeps it alive, why members came in.

Answers the recurring question "which symbols can WE remove?" with three joined views:

  1. placement  - every placed section (.text.X / .literal.X / .rodata.X ...) grouped by
                  symbol, with the owning object/archive member and the summed bytes. This
                  is the flash truth because -Wl,--gc-sections is in the link line.
  2. inclusion  - for each archive member, the referrer(s) that pulled it into the link
                  ("Archive member included to satisfy reference by file (symbol)").
  3. xref       - the Cross Reference Table (--cref): every referrer of every symbol.

Usage:
    python3 map_census.py <map> [--ours-symbols] [--top N] [--member SUBSTR] [--json OUT]

--ours-symbols  only symbols that at least one of our own objects references, ranked by bytes.
--member SUBSTR placement groups whose owning member contains SUBSTR (e.g. compiler_builtins).
"""
import re
import sys
import json
import collections

SEC = re.compile(r'^\s(\.\S+)$')
PLC = re.compile(r'^\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S+)$')
INC_MEMBER = re.compile(r'^(\S.*\.a\(\S+\)|\S.*\.o)\s*$')
INC_REF = re.compile(r'^\s+(\S+)?\s*\(([^)]+)\)\s*$')
# sections that are not code-or-data of one symbol: debug info, merged strings, tables
SKIP = re.compile(r'(^debug_|\.str\d*\.|^embedded$|^\.comment$|_cst|^swift)')
KEEP = ('.text.', '.literal.', '.rodata.', '.bss.', '.data.', '.noinit.')


def kind(path):
    """Classify a referrer/owner path into who owns it."""
    if 'compiler_builtins' in path:
        return 'rust-builtins'
    if 'libpocketjs' in path or '/.cache/native/' in path:
        return 'rust'
    if 'quickjs' in path:
        return 'quickjs'
    if 'picolibc' in path or re.search(r'/(libc|libm|libgcc)\.[ao]\(', path):
        return 'libc'
    if 'components/opus' in path or '/libopus' in path:
        return 'opus'
    if '/main/' in path or path.startswith('esp-idf/main/') or 'main/libmain' in path:
        return 'ours'
    if 'esp-idf/' in path or '/opt/esp-idf/' in path:
        return 'idf'
    return 'other'


def parse(path):
    lines = open(path, errors='replace').read().splitlines()

    def find(prefix):
        for i, l in enumerate(lines):
            if l.startswith(prefix):
                return i
        return None

    i_inc = find('Archive member included')
    i_plc = find('Linker script and memory map')
    i_cref = find('Cross Reference Table')
    if i_inc is None or i_plc is None or i_cref is None:
        sys.exit('map is missing one of the three sections')

    # 1. placement: symbol -> bytes, owning members
    place = collections.defaultdict(lambda: {'bytes': 0, 'members': collections.Counter()})
    sec = None
    for l in lines[i_plc:i_cref]:
        m = SEC.match(l)
        if m:
            sec = m.group(1)
            continue
        m = PLC.match(l)
        if m and sec and sec.startswith(KEEP) and not SKIP.search(sec):
            size = int(m.group(1), 16)
            member = m.group(2)
            sym = sec.lstrip('.')          # .text.foo -> text.foo
            sym = sym.split('.', 1)[1] if '.' in sym else sym
            e = place[sym]
            e['bytes'] += size
            e['members'][member] += size

    # 2. inclusion: member -> (referrers, symbol)
    incl = {}
    cur = None
    for l in lines[i_inc:i_plc]:
        if not l.strip():
            continue
        m = INC_REF.match(l)
        if m and cur:
            ref = m.group(1) or '(unattributed)'
            incl[cur]['refs'].append((ref, m.group(2)))
            continue
        if l[0] != ' ' and ('.a(' in l or l.endswith('.o')):
            cur = l.strip()
            incl[cur] = {'refs': []}

    # 3. xref: symbol -> referrers
    xref = {}
    cur = None
    for l in lines[i_cref:]:
        if not l.strip() or l.startswith('Symbol'):
            continue
        if l[0] != ' ':
            cur = l.strip().split()[0]
            xref.setdefault(cur, [])
        elif cur:
            xref[cur].append(l.strip().split()[0])
    return place, incl, xref


def main():
    path = sys.argv[1]
    ours_only = '--ours-symbols' in sys.argv
    lib_only = '--library-targets' in sys.argv
    top = 40
    if '--top' in sys.argv:
        top = int(sys.argv[sys.argv.index('--top') + 1])
    member_filter = None
    if '--member' in sys.argv:
        member_filter = sys.argv[sys.argv.index('--member') + 1]
    place, incl, xref = parse(path)

    print(f'map {path}: {len(place)} placed symbols, {len(incl)} archive members, {len(xref)} xref symbols')

    rows = []
    for sym, e in place.items():
        if member_filter and not any(member_filter in m for m in e['members']):
            continue
        refs = xref.get(sym, [])
        ks = collections.Counter(kind(r) for r in refs)
        owner = collections.Counter(kind(m) for m in e['members'])
        ours = ks.get('ours', 0)
        if ours_only and not ours:
            continue
        # a library target: library code that our objects reference (drop our ref -> maybe drop it)
        if lib_only and (not ours or 'ours' in owner):
            continue
        rows.append((e['bytes'], sym, ks, owner, e['members']))
    rows.sort(reverse=True, key=lambda r: r[0])

    title = 'library code our objects reference and others keep' if lib_only else \
            'placed symbols referenced by us' if ours_only else 'placed symbols'
    print(f'\n== {title} (top {top}) ==')
    print(f'{"bytes":>7}  {"own":<12} {"kinds":<30} symbol')
    for b, sym, ks, owner, members in rows[:top]:
        k = ','.join(f'{v}x{a}' for a, v in ks.most_common())
        o = ','.join(f'{v}x{a}' for a, v in owner.most_common())
        print(f'{b:>7}  {o:<12} {k:<30} {sym[:64]}')

    sole = [(b, s, ks) for b, s, ks, _, _ in rows if list(ks) == ['ours'] and b >= 64]
    print(f'\n== sole-referrer (our code only), >=64 B: {len(sole)} symbols, {sum(b for b,_,_ in sole)} B total ==')
    for b, s, ks in sorted(sole, reverse=True)[:top]:
        print(f'{b:>7}  {s[:70]}')

    # members pulled in for our references (any referrer ours), with their placed bytes
    member_bytes = collections.Counter()
    for sym, e in place.items():
        for m, b in e['members'].items():
            member_bytes[m] += b
    ours_pulled = []
    for mem, d in incl.items():
        refs = d['refs']
        ks = collections.Counter(kind(r) for r, _ in refs)
        if ks.get('ours'):
            ours_pulled.append((member_bytes[mem], ks.get('ours'), len(refs), mem, refs))
    print(f'\n== archive members pulled in with one of our objects as a referrer: {len(ours_pulled)} '
          f'({sum(1 for b, o, n, _, _ in ours_pulled if o == n)} of them ours-only) ==')
    for b, ours, n, mem, refs in sorted(ours_pulled, reverse=True)[:top]:
        why = ', '.join(f'{r.split("/")[-1]}({s})' for r, s in refs[:2])
        flag = 'OURS-ONLY' if ours == n else f'{ours}/{n}'
        print(f'  {b:>7} B  {flag:<10} {mem.split("/")[-1][:76]}  <- {why[:96]}')

    if '--json' in sys.argv:
        out = sys.argv[sys.argv.index('--json') + 1]
        json.dump(
            {
                'placed': {s: {'bytes': e['bytes'], 'members': dict(e['members'])} for s, e in place.items()},
                'included': {m: [{'ref': r, 'symbol': s} for r, s in d['refs']] for m, d in incl.items()},
                'xref': {s: r for s, r in xref.items()},
            },
            open(out, 'w'),
        )
        print(f'\njson -> {out}')


if __name__ == '__main__':
    main()
