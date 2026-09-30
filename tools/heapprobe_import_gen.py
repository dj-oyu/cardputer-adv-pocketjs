"""The same program three ways, for comparing evaluation peaks
(docs/vm/eval-peak.md section 9): one global script, three global scripts read
with pocket.app.load(), and three ES modules reached by static import.

  python tools/heapprobe_import_gen.py OUTDIR [--parts N] [--app ID]

Why a generated program rather than DERBY WATCH: DERBY WATCH's parts write each
other's top-level `let`s, and an imported binding is read-only, so it cannot be
cut into modules without being rewritten (section 3.3). This program is shaped
like app code -- functions with loops and branches, closures, object literals
with methods, number tables, string constants -- about 10 KB a part, so three
parts are near DERBY WATCH's 32 KB. The parts are byte-identical in the three
forms except for one trailing `export { pN_run };` line in the modules, so the
difference in the peaks is the evaluation mechanism.

Writes to OUTDIR:
  imp_all.js            the parts concatenated, then TAIL (one script)
  imp_s1.js .. imp_sN.js  each part as a global script
  imp_m1.mjs .. imp_mN.mjs  each part as a module
  imp_chunks.txt        `app ID`, then impsK imp_sK.js and impmK imp_mK.mjs
and prints the two entries (load, import) the device probe embeds in C.
"""
import argparse
from pathlib import Path

NF, NC = 16, 8   # functions and closure factories a part

TAIL = ("const v = {runs}; console.log('HP_EVAL ok ' + v); "
        "globalThis.frame = () => {{}};\n")


def part(k):
    """About 10 KB of app-shaped code for part k; its names start with pK_."""
    p = f'p{k}_'
    out = [f"'use strict';\n// Part {k} of the generated import/load/script comparison.\n"]
    for t in range(4):
        vals = ', '.join(str((k * 131 + t * 17 + i * 29) % 251) for i in range(48))
        out.append(f'const {p}T{t} = [{vals}];\n')
    out.append(f"const {p}NAMES = ['alpha{k}', 'bravo{k}', 'charlie{k}', 'delta{k}', "
               f"'echo{k}', 'foxtrot{k}'];\n")
    for f in range(NF):
        t = f % 4
        out.append(
            f'function {p}f{f}(a, b) {{\n'
            f'  let s = {f}, m = 1;\n'
            f'  for (let i = 0; i < {p}T{t}.length; i++) {{\n'
            f'    const x = {p}T{t}[i] ^ (a + i);\n'
            f'    if (x & 1) s = (s + x * b) % 65521; else s = (s * 3 + x) % 65521;\n'
            f'    m = (m * 7 + s) & 0xffff;\n'
            f'  }}\n'
            f'  const label = {p}NAMES[(s + m) % {p}NAMES.length];\n'
            f'  return label.length > 5 ? s ^ m : s + m;\n'
            f'}}\n')
    for c in range(NC):
        out.append(
            f'function {p}make{c}(seed) {{\n'
            f'  let state = seed + {c};\n'
            f'  const step = d => {{ state = (state * 1103515245 + d + 12345) & 0x7fffffff; return state; }};\n'
            f'  return {{ next: () => step({c}) % 1000, skip(n) {{ for (let i = 0; i < n; i++) step(i); return this; }},\n'
            f'           get value() {{ return state; }} }};\n'
            f'}}\n')
    out.append(f'const {p}scene = {{\n')
    for m in range(10):
        out.append(
            f'  layer{m}: {{ x: {m * 3}, y: {m * 5 % 17}, w: {40 + m}, h: {12 + m % 4}, '
            f"tag: 'L{k}.{m}',\n"
            f'    draw(t) {{ const r = {p}f{m % NF}(this.x + t, this.y); '
            f'return r % (this.w * this.h); }} }},\n')
    out.append('};\n')
    out.append(
        f'function {p}run() {{\n'
        f'  let acc = 0;\n'
        f'  for (let f = 0; f < {NF}; f++) acc = (acc + [{", ".join(f"{p}f{i}" for i in range(NF))}][f](f, {k})) % 1000003;\n'
        f'  const gens = [{", ".join(f"{p}make{c}" for c in range(NC))}].map((g, i) => g(i + acc % 13));\n'
        f'  for (const g of gens) acc = (acc + g.skip(3).next()) % 1000003;\n'
        f'  for (const key in {p}scene) acc = (acc + {p}scene[key].draw(acc & 7)) % 1000003;\n'
        f'  return acc;\n'
        f'}}\n')
    return ''.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('outdir')
    ap.add_argument('--parts', type=int, default=3)
    ap.add_argument('--app', default='local.hello')
    a = ap.parse_args()
    out = Path(a.outdir)
    out.mkdir(parents=True, exist_ok=True)
    n = a.parts
    runs = ' + '.join(f'p{k}_run()' for k in range(1, n + 1))
    tail = TAIL.format(runs=runs)
    parts = [part(k) for k in range(1, n + 1)]

    def write(name, text):
        path = out / name
        if not path.exists() or path.read_text(encoding='utf-8') != text:
            path.write_text(text, encoding='utf-8', newline='\n')

    write('imp_all.js', ''.join(parts) + tail)
    lst = [f'app {a.app}']
    for k, body in enumerate(parts, 1):
        write(f'imp_s{k}.js', body)
        write(f'imp_m{k}.mjs', body + f'export {{ p{k}_run }};\n')
        lst += [f'imps{k} imp_s{k}.js', f'impm{k} imp_m{k}.mjs']
    write('imp_chunks.txt', '\n'.join(lst) + '\n')
    load = ''.join(f"pocket.app.load('imps{k}');" for k in range(1, n + 1)) + tail
    imp = ''.join(f"import {{ p{k}_run }} from 'impm{k}';" for k in range(1, n + 1)) + tail
    write('imp_entry_load.js', load)
    write('imp_entry_import.mjs', imp)
    for k, body in enumerate(parts, 1):
        print(f'part {k}: {len(body.encode())} B')
    print(f'all: {len("".join(parts).encode()) + len(tail)} B')


if __name__ == '__main__':
    main()
