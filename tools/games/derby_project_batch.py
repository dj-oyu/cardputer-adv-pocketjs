"""Generate the opt-in PAN pose/projection boundary; leave sorting in JS."""
from pathlib import Path

def once(text,old,new):
    if text.count(old)!=1:raise ValueError('projection batch source drift: '+old[:80])
    return text.replace(old,new,1)

def instrument(source):
    source=source.replace('\r\n','\n')
    source=once(source,'const S = pocket.derby;','''const S = pocket.derby;
// Persistent output planes; no per-point pose/project result arrays.
const pr = new Float64Array(32), pp = new Float64Array(24),
  pg = [200,400,600,800,1000,1000], pw = [DFR,DFR,DFR,DFR,DFR,DNR];''')
    old='''  for (let m = 200; m <= D; m += 200) {
    const p = pj(pose(m, DFR)), r = 1 / p[1], x = p[0] * r, e = m === D, n = e ? pj(pose(m, DNR)) : p, y = n[0] / n[1];
    if (p[1] > .02 && n[1] > .02 && x > -40 && x < 280 && y > -400 && y < 640)
      dr('pole', x, 28 + 6 * r, 28 + (e ? 2 : 3.4) * r, (e ? .55 : .3) * r, y, 28 + 6 / n[1], x, 28 + 6 * r);
  }
  const ps = xs.map((x, l) => pj(pose(x - 12.5 * U, lz[l])));
  zo.sort((a, b) => ps[b][1] - ps[a][1]);
  for (const l of zo) {
    const p = ps[l], r = 1 / p[1], X = p[0] * r;
    if (p[1] > .02 && X > -160 && X < 400) rin(l, X, r * U, 28 + 6 * r, ph[l]);
  }
'''
    new='''  S.project(pg, pw, 0, pp);
  for (let j = 0; j < 5; j++) {
    const p = j * 4, e = j === 4, n = e ? 20 : p, r = pp[p+3], x = pp[p+2], y = pp[n] / pp[n+1];
    if (pp[p+1] > .02 && pp[n+1] > .02 && x > -40 && x < 280 && y > -400 && y < 640)
      dr('pole', x, 28 + 6 * r, 28 + (e ? 2 : 3.4) * r, (e ? .55 : .3) * r, y, 28 + 6 / pp[n+1], x, 28 + 6 * r);
  }
  S.project(xs, lz, 12.5 * U, pr);
  zo.sort((a, b) => pr[b*4+1] - pr[a*4+1]);
  for (const l of zo) {
    const p = l * 4, r = pr[p+3], X = pr[p+2];
    if (pr[p+1] > .02 && X > -160 && X < 400) rin(l, X, r * U, 28 + 6 * r, ph[l]);
  }
'''
    return once(source,old,new)

def generate(app_dir:Path):
    path=app_dir/'derby_pan.js';path.write_text(instrument(path.read_text()))
