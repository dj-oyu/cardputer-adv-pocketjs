// What one pocket.kasane.shiki.draw() costs, split into its stages
// (docs/kasane/shiki-bundle.md): README.md. Started by the USB broker.
'use strict';
const S = pocket.kasane.shiki, D = pocket.shikiDebug, N = 200, ROUNDS = 5;
const asm = (inputs, code) => {
  const head = 12 + inputs.length, body = (head + 3) & ~3;
  const b = new Uint8Array(body + 12 * code.length), dv = new DataView(b.buffer);
  b.set([83, 75, 73, 49, 1, 0, code.length, inputs.length, 0, 0, 0, 0]);
  b.set(inputs, 12);
  code.forEach((i, k) => { const o = body + 12 * k; b.set(i.slice(0, 4), o); dv.setInt32(o + 4, i[4] || 0, true); dv.setUint16(o + 8, i[5] || 0, true); });
  return b;
};
const h0 = S.register(asm([], [[0, 0, 0, 0, 1]]));
const h8 = S.register(asm([16, 16, 16, 16, 16, 16, 16, 16], [[0, 0, 0, 0, 1]]));
const hp = S.register(asm([16, 16], [[1, 0, 0], [1, 1, 1], [8, 0, 0, 1, 0, 2016]]));
const arr = [1, 2, 3, 4, 5, 6, 7, 8];
const noop = function (x) { return x; };
const H0 = new Uint32Array(N).fill(h0), H8 = new Uint32Array(N).fill(h8), Z = new Float64Array(0);
const R8 = new Float64Array(N * 8), F8 = new Int32Array(N * 8);
for (let i = 0; i < N * 8; i++) { R8[i] = 1 + i % 8; F8[i] = (1 + i % 8) * 65536; }
const cases = [
  ['loop', () => { for (let i = 0; i < N; i++) noop(i); }],
  ['native', () => { for (let i = 0; i < N; i++) D.cycles(); }],
  ['draw0', () => { for (let i = 0; i < N; i++) S.draw(h0); }],
  ['draw8', () => { for (let i = 0; i < N; i++) S.draw(h8, 1, 2, 3, 4, 5, 6, 7, 8); }],
  ['draw8arr', () => { for (let i = 0; i < N; i++) S.draw(h8, arr); }],
  ['point', () => { for (let i = 0; i < N; i++) S.draw(hp, 120, 60); }],
  // drawMany (prototype): the same N draws in one call.
  ['many0', () => S.drawMany(H0, Z, 0)],
  ['many8', () => S.drawMany(H8, R8, 8)],
  ['many8fixed', () => S.drawMany(H8, F8, 8)],
];
let done = false;
globalThis.frame = function () {
  if (done) return;
  done = true;
  console.log('DC BEGIN n=' + N + ' rounds=' + ROUNDS + ' BUILD ' + D.benchBuild());
  for (const [name, run] of cases) {
    let best = Infinity, prof = null;
    for (let r = 0; r < ROUNDS; r++) {
      S.beginFrame(0);
      D.drawProfile(true);
      const t0 = D.cycles();
      run();
      const dt = (D.cycles() - t0) >>> 0;
      const p = D.drawProfile(false);
      if (dt < best) { best = dt; prof = p; }
    }
    const per = v => (v / N).toFixed(1);
    console.log('DC ' + name + ' total=' + per(best) + ' parse=' + per(prof.parse) + ' lookup=' + per(prof.lookup) +
      ' engine=' + per(prof.engine) + ' copy=' + per(prof.copy) + ' n=' + prof.n);
  }
  S.commit();
  console.log('DC END');
};
