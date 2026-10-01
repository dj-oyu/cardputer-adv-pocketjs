"""Split an app written as one immediately invoked function into global scripts.

The evaluation heap probe's chunk variants (docs/vm/spread-eval-oom.md,
POCKET_HEAPPROBE only). QuickJS keeps every JSFunctionDef of one script alive
until the whole script is parsed, so an app's parse peak is one number for
the whole source; evaluated as N scripts in one realm it is paid N times,
each over what the earlier ones left. This makes those N scripts
mechanically, from apps/derby/derby_watch.js, without touching the app:

  - the `(function () {` / `})();` wrapper and the host-only early return
    (`if (typeof pocket === 'undefined') return;`) are dropped; every chunk
    starts with the wrapper's 'use strict'. The body's top-level const/let
    become the realm's global lexical bindings, visible to the later
    scripts (and to the earlier scripts' functions, once they exist);
  - chunks are cut at top-level statement boundaries (a two-space indented
    line after a complete two-space statement), nearest to equal byte sizes;
  - line numbers are kept by padding with blank lines, so an error names the
    same line as in the app.

  python tools/heapprobe_split.py SOURCE N OUTDIR [--flat]

writes OUTDIR/<stem>_c1.js .. _cN.js (--flat: N is ignored and one file,
OUTDIR/<stem>_flat.js, holds the whole unwrapped body).
"""
import re
import sys
from pathlib import Path


def unwrap(lines):
    start = next(i for i, l in enumerate(lines) if l.startswith('(function () {'))
    end = max(i for i, l in enumerate(lines) if l.startswith('})();'))
    body = []
    for i, l in enumerate(lines):
        if i <= start or i >= end or l.strip() in ("'use strict';",
                                                   "if (typeof pocket === 'undefined') return;"):
            body.append('')
        else:
            body.append(l)
    return body


def boundaries(body):
    """Indices where a new top-level statement may start a chunk."""
    out, prev = [], None
    for i, l in enumerate(body):
        if not l.strip():
            continue
        top = re.match(r'  \S', l) and not re.match(r'  [}\])\.+\-*/?:,&|]', l)
        if top and prev is not None:
            p = body[prev]
            if (re.match(r'  \S', p) or re.match(r'  //', p)) and \
               (p.rstrip().endswith((';', '}')) or p.lstrip().startswith('//')):
                out.append(i)
        prev = i
    return out


def main():
    src, n, outdir = Path(sys.argv[1]), int(sys.argv[2]), Path(sys.argv[3])
    flat = '--flat' in sys.argv
    lines = src.read_text(encoding='utf-8').split('\n')
    body = unwrap(lines)
    outdir.mkdir(parents=True, exist_ok=True)
    head = "'use strict';"
    if flat:
        body[0] = head
        (outdir / f'{src.stem}_flat.js').write_text('\n'.join(body), encoding='utf-8', newline='\n')
        return
    cand = boundaries(body)
    size = [len(l) + 1 for l in body]
    total = sum(size)
    cuts = []
    for k in range(1, n):
        want = total * k / n
        acc, best = 0, None
        pos = {}
        for i in range(len(body)):
            pos[i] = acc
            acc += size[i]
        best = min((c for c in cand if not cuts or c > cuts[-1]), key=lambda c: abs(pos[c] - want))
        cuts.append(best)
    edges = [0] + cuts + [len(body)]
    for k in range(n):
        a, b = edges[k], edges[k + 1]
        # Blank lines stand for the other chunks, so line numbers match the app.
        chunk = [''] * a + body[a:b]
        chunk[0] = head if not chunk[0].strip() else head + ' ' + chunk[0].lstrip()
        text = '\n'.join(chunk)
        (outdir / f'{src.stem}_c{k + 1}.js').write_text(text, encoding='utf-8', newline='\n')
        print(f'chunk {k + 1}: lines {a + 1}..{b} bytes {sum(size[a:b])}')


if __name__ == '__main__':
    main()
