"""Opt-in host experiment: keep actual PAN pose/projection/sort in JS.

Only replace the visible runner submissions. Shipping sources stay untouched.
"""
from pathlib import Path


def instrument(source: str) -> str:
    source = source.replace('\r\n', '\n')
    begin = "  for (const l of zo) {\n    const p = pj(pose(xs[l] - 12.5 * U, lz[l]))"
    end = "  S.ser(live.prail, DNR, k[4], 0, L, 0xffff, 4.9);"
    a, b = source.index(begin), source.index(end)
    original = source[a:b]
    expected = "if (p[1] > .02 && X > -160 && X < 400) rin(l, X, r * U, 28 + 6 * r, ph[l]);"
    if original.count(expected) != 1:
        raise ValueError('PAN runner source changed')
    replacement = original.replace(expected, """if (p[1] > .02 && X > -160 && X < 400) {
      const i = rn++ * 5;
      rb[i] = live['r' + l] || 0; rb[i+1] = X; rb[i+2] = r * U;
      rb[i+3] = 28 + 6 * r; rb[i+4] = ph[l];
    }""")
    return source[:a] + "  let rn = 0;\n" + replacement + "  S.runners(rb.buffer, rn);\n" + source[b:] + "\nconst rb = new Float64Array(40);\n"


def generate(app_dir: Path) -> None:
    path = app_dir / 'derby_pan.js'
    path.write_bytes(instrument(path.read_text(encoding='utf-8')).encode('utf-8'))
