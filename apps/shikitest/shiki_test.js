// Shiki on-device differential test (README.md). The case generator is
// shared: tools/kasane_ir/shiki_device.mjs loads this file without pocket.
'use strict';
globalThis.shikiGen = function (seed) {
  let st = seed >>> 0 || 1;
  const rnd = () => { st ^= st << 13; st >>>= 0; st ^= st >>> 17; st ^= st << 5; st >>>= 0; return st / 4294967296; };
  const ri = (a, b) => a + Math.floor(rnd() * (b - a + 1)), pick = a => a[Math.floor(rnd() * a.length)];
  const I = [0, 1, -1, 32767, 32768, -32768, -32769, 65535, 65536, -65536, 2147483647, -2147483648, 16384, 49152, 46341];
  const crd = () => pick([-481, -480, 0, 120, 239, 720, 721, ri(-500, 740)]) * 65536 + pick([0, 1, 32767, 32768, 65535, ri(0, 65535)]);
  const reg = () => ri(0, 15);
  return function () {
    const nin = ri(0, 8), inq = [], code = [], open = [];
    for (let k = 0; k < nin; k++) inq.push(pick([16, 16, 0, 24, 30]));
    const n = ri(4, 48), heavy = rnd() < .1;
    while (code.length + open.length < n) {
      const r = rnd();
      if (r < .1 && open.length < 8) {
        if (rnd() < .7) code.push([5, 0, 0, 0, heavy ? ri(100, 255) : ri(1, 9), 0]);
        else { const c = reg(); code.push([0, c, 0, 0, pick([0, 1, 255, 256, -1]), 0], [10, 0, c, 0, 0, 0]); }
        open.push(1);
      } else if (r < .16 && open.length) { code.push([6, 0, 0, 0, 0, 0]); open.pop(); }
      else if (r < .19 && open.length) code.push([11, 0, reg(), reg(), 0, 0]);
      else if (r < .32) code.push([0, reg(), 0, 0, rnd() < .5 ? crd() : rnd() < .5 ? pick(I) : ri(-70000, 70000), 0]);
      else if (r < .37 && nin) code.push([1, reg(), ri(0, nin - 1), 0, 0, 0]);
      else if (r < .6) {
        const op = pick([2, 16, 17, 3, 3, 18, 19, 20, 21, 22, 4]), a = reg();
        if (op >= 19 && op <= 22 && rnd() < .5) code.push([0, a, 0, 0, pick([32768, -32769, 70000, -2147483648]), 0]);
        code.push([op, reg(), a, op === 17 || op === 18 || op === 22 || op === 4 ? 0 : reg(),
          op === 18 ? ri(-31, 31) : op === 22 ? ri(-15, 14) : 0, op === 3 ? ri(0, 62) : op === 21 ? ri(0, 31) : 0]);
      } else {
        const op = pick([7, 8, 9, 9, 12, 13]);
        code.push([op, op > 11 ? reg() : 0, reg(), reg(), 0, op === 8 || op === 9 ? ri(0, 65535) : 0]);
      }
    }
    while (open.length) { code.push([6, 0, 0, 0, 0, 0]); open.pop(); }
    const narg = rnd() < .3 ? 1 : 0, args = [], argq = [], pat = [];
    if (narg) {
      const pc = code.findIndex(i => i[0] === 0);
      if (pc >= 0) { pat.push([pc, 0, 0, 0]); argq.push(pick([16, 0, 24])); args.push(ri(-500, 740) + pick([0, .5, -.5, 1 / 65536])); }
    }
    const head = 12 + nin + argq.length + 4 * pat.length, body = (head + 3) & ~3;
    const b = new Uint8Array(body + 12 * code.length), dv = new DataView(b.buffer);
    b.set([83, 75, 73, 49, 1, 0, code.length, nin, argq.length, 0, pat.length, 0]);
    b.set(inq, 12); b.set(argq, 12 + nin);
    pat.forEach((p, k) => b.set(p, 12 + nin + argq.length + 4 * k));
    code.forEach((i, k) => { const o = body + 12 * k; b.set(i.slice(0, 4), o); dv.setInt32(o + 4, i[4], true); dv.setUint16(o + 8, i[5], true); });
    if (rnd() < .2) { const k = ri(0, b.length - 1); b[k] ^= 1 << ri(0, 7); }
    const inp = [];
    for (let k = rnd() < .1 ? ri(0, 10) : nin; k > 0; k--)
      inp.push(pick([ri(-600, 800), ri(-600, 800) + .5, -(ri(0, 600) + .5), ri(-1e6, 1e6) / 65536, 1e12, -1e12, 0]));
    let hex = '';
    for (let k = 0; k < b.length; k++) hex += (b[k] < 16 ? '0' : '') + b[k].toString(16);
    const line = ('CASE ' + hex + ' ARGS ' + args.length + ' ' + args.join(' ') + ' INPUTS ' + inp.length + ' ' + inp.join(' ')).replace(/  +/g, ' ');
    return {b: b, args: args, inp: inp, line: line};
  };
};
globalThis.shikiHash = function (s) {
  let h = 2166136261;
  for (let k = 0; k < s.length; k++) h = Math.imul(h ^ s.charCodeAt(k), 16777619);
  return (h >>> 0).toString(16).padStart(8, '0');
};
if (typeof pocket !== 'undefined') {
  const D = pocket.shikiDebug, N = 600, SEED = 1, next = shikiGen(SEED);
  let i = 0;
  console.log('SK BEGIN seed ' + SEED + ' cases ' + N);
  globalThis.frame = function () {
    for (let k = 0; k < 20 && i < N; k++, i++) {
      const c = next();
      console.log('SK ' + i + ' ' + shikiHash(c.line) + ' ' + D.run(c.b, c.args, c.inp));
    }
    if (i === N) { console.log('SK END ' + N); i++; }
  };
}
