// Shiki data regions (v1.3) on-device differential test (README.md). The
// generator is shared: tools/kasane_ir/shiki_device.mjs --data loads this file.
'use strict';
globalThis.shikiDataGen = function (seed) {
  let st = seed >>> 0 || 1;
  const rnd = () => { st ^= st << 13; st >>>= 0; st ^= st >>> 17; st ^= st << 5; st >>>= 0; return st / 4294967296; };
  const ri = (a, b) => a + Math.floor(rnd() * (b - a + 1)), pick = a => a[Math.floor(rnd() * a.length)];
  const reg = () => ri(0, 15), sk = () => pick([0, 0, -1, 1, 8, -8, 16, -16, 31, -31, ri(-31, 31)]);
  const E = [() => pick([-32768, -1, 0, 1, 32767, ri(-32768, 32767), ri(-300, 300)]),
    () => pick([0, 32767, 32768, 65535, ri(0, 65535)]),
    () => pick([-2147483648, 2147483647, -1, 0, 65536, ri(-2147483648, 2147483647), ri(-70000, 70000)])];
  return function () {
    const bad = rnd() < .25, no = () => bad && rnd() < .08, nr = ri(1, 4), dec = [], pas = [];
    for (let k = 0; k < nr; k++) dec.push([ri(0, 2), pick([0, 8, 16, 30, ri(0, 30)]), rnd() < .4 ? 1 : ri(1, 8), no() ? 0 : 1]);
    if (!dec.some(d => d[2] === 1)) dec[0][2] = 1;
    for (let k = 0; k < nr; k++) {
      const d = dec[k];
      if (k && rnd() < .1) { let j = ri(0, k - 1); if (typeof pas[j] === 'number') j = pas[j]; d[0] = dec[j][0]; d[1] = dec[j][1]; d[2] = dec[j][2]; pas.push(j); continue; }
      const n = pick([1, 2, 3, 8, 9, ri(1, 30)]) * d[2], v = [];
      for (let e = 0; e < n; e++) v.push(E[d[0]]());
      pas.push({t: no() ? (d[0] + 1) % 3 : d[0], q: d[1], k: d[2], v: no() ? [] : v});
    }
    const rec = a => { const g = typeof pas[a] === 'number' ? pas[pas[a]] : pas[a]; return g.v.length / g.k | 0; };
    const code = [], open = [], n = ri(6, 48), k1 = [];
    dec.forEach((d, i) => { if (d[2] === 1) k1.push(i); });
    while (code.length + open.length < n) {
      const r = rnd(), ins = open.filter(o => o >= 0);
      if (r < .15 && open.length < 7) {
        const a = ins.length && rnd() < .2 ? pick(ins) : ri(0, nr - 1);
        if (rnd() < .2) { code.push([5, 0, 0, 0, pick([2, 255]), 0]); open.push(-1); }
        if (rnd() < .5) code.push([23, 0, a, 0, 0, 0]);
        else { const c = reg(), m = rec(a); code.push([0, c, 0, 0, pick([0, 1, m, m, m - 1, m + 1, -1]), 0], [23, 0, a, c, 0, 1]); }
        open.push(a);
      } else if (r < .27 && open.length) { code.push([6, 0, 0, 0, 0, 0]); open.pop(); }
      else if (r < .3 && open.length) code.push([11, 0, reg(), reg(), 0, 0]);
      else if (r < .5 && ins.length) {
        const a = pick(ins), k = dec[a][2], m = no() ? 9 : ri(1, k), b = ri(0, Math.max(0, k - m));
        code.push([rnd() < .5 && dec[a][3] ? 25 : 24, ri(0, 16 - m), a, b, sk(), m]);
      } else if (r < .57 && k1.length) {
        const i = reg();
        if (rnd() < .6) code.push([0, i, 0, 0, pick([-1, 0, 1, -65536, 2147483647, ri(-5, 40), ri(-5, 40) * 65536]), 0]);
        code.push([26, reg(), pick(k1), i, sk(), pick([0, 16, 31, ri(0, 31)])]);
      } else if (r < .8) {
        const op = pick([2, 16, 17, 3, 18, 19, 21, 22]), a = reg();
        code.push([op, reg(), a, op === 17 || op === 18 || op === 22 ? 0 : reg(),
          op === 18 ? sk() : op === 22 ? ri(-15, 14) : 0, op === 3 ? ri(0, 62) : op === 21 ? ri(0, 31) : 0]);
      } else if (r < .9) code.push([0, reg(), 0, 0, pick([0, 1, -1, 32768, 2147483647, ri(-70000, 70000), ri(0, 239) * 65536]), 0]);
      else code.push([8, 0, reg(), reg(), 0, ri(0, 65535)]);
    }
    while (open.length) { code.push([6, 0, 0, 0, 0, 0]); open.pop(); }
    const head = 12 + 4 * nr, body = (head + 3) & ~3;
    const b = new Uint8Array(body + 12 * code.length), dv = new DataView(b.buffer);
    b.set([83, 75, 73, 49, 1, 0, code.length, 0, 0, nr, 0, 0]);
    dec.forEach((d, i) => b.set(d, 12 + 4 * i));
    code.forEach((i, k) => { const o = body + 12 * k; b.set(i.slice(0, 4), o); dv.setInt32(o + 4, i[4], true); dv.setUint16(o + 8, i[5], true); });
    if (rnd() < .1) { const k = ri(0, b.length - 1); b[k] ^= 1 << ri(0, 7); }
    let hex = '';
    for (let k = 0; k < b.length; k++) hex += (b[k] < 16 ? '0' : '') + b[k].toString(16);
    const dl = pas.map(g => typeof g === 'number' ? 'ALIAS ' + g : g.t + ' ' + g.q + ' ' + g.k + ' ' + g.v.length + (g.v.length ? ' ' + g.v.join(' ') : ''));
    return {b: b, data: pas, line: 'CASE ' + hex + ' ARGS 0 INPUTS 0 DATA ' + nr + ' ' + dl.join(' ')};
  };
};
globalThis.shikiHash = function (s) {
  let h = 2166136261;
  for (let k = 0; k < s.length; k++) h = Math.imul(h ^ s.charCodeAt(k), 16777619);
  return (h >>> 0).toString(16).padStart(8, '0');
};
if (typeof pocket !== 'undefined') {
  const D = pocket.shikiDebug, N = 600, SEED = 1, next = shikiDataGen(SEED);
  let i = 0, out = '';
  console.log('SD BEGIN seed ' + SEED + ' cases ' + N);
  globalThis.frame = function () {
    const t0 = Date.now();
    while (i < N && Date.now() - t0 < 60) {
      const c = next();
      out += (out ? ' ' : 'SD ' + i + ' ') + shikiHash(c.line) + ' ' + D.run(c.b, [], [], c.data);
      if (++i % 4 === 0 || i === N) { console.log(out); out = ''; }
    }
    if (i === N) { console.log('SD END ' + N); i++; }
  };
}
