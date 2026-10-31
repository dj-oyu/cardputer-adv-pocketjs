// The @shiki compiler's data regions (shiki-spec.md v1.3 sec.8.2, 8.3)
// against the AST evaluation of the same rules (host, Node only).
//
//   node tools/kasane_ir/test_shiki_data_compile.mjs [--vectors N] [--seed S]
//
// Each plan below is compiled (shiki_compile.mjs) and run through the host
// reference engine (shiki_ref.mjs, equal to the C engine bit for bit) on
// random region contents and inputs; shiki_fix.mjs evaluates the same AST on
// copies of the same contents. Status, segments and every region's contents
// after the run must agree. The step limit is the engine's alone, so a
// vector the engine stops on it is counted and skipped. The second list is
// plans the compiler must refuse (sec.8.3 rule 1 and 9, sec.8.2), each with
// the message it must give.
import {findPlans, planAst} from './plan_js.mjs';
import {qdecls} from './shiki_q.mjs';
import {Fail, FIXM, emitFix} from './shiki_fix.mjs';
import {compileShiki} from './shiki_compile.mjs';
import {register, draw, addSeg, makeRegion} from './shiki_ref.mjs';

const argv = process.argv.slice(2);
const opt = (n, d) => { const i = argv.indexOf(n); return i < 0 ? d : Number(argv[i + 1]); };
const VECTORS = opt('--vectors', 300), SEED = opt('--seed', 1);

const PLANS = `
/** @shiki points inputs: dx, dy
 *  @data pts: i16 q0 record 2 */
function points(colour) {
  for (const [x, y] of pts) { plot(x + dx, y + dy, colour); }
}

/** @shiki seats inputs: dx, n
 *  @q dx=8
 *  @data seats: i16 q8 record 3 write */
function seats() {
  for (const [a, b, c] of seats.take(n)) {
    seats.put(a + dx, b, c * 2);
    move(a, b);
    line(a + 4, b, 2016);
  }
}

/** @shiki swap inputs: k
 *  @data pairs: i32 q16 record 2 write */
function swap() {
  for (const [a, b] of pairs) {
    pairs.put(b, a * k);
  }
}

/** @shiki table inputs: t, x0
 *  @data wave: i16 q14 record 1
 *  @data ramp: u16 q0 record 1 */
function table() {
  move(x0, 60);
  for (let i = 0; i < 40; i++) {
    line(x0 + i * 4, 60 + wave[t + i * 0.25] * 40, ramp[i]);
  }
}

/** @shiki nested inputs: lim
 *  @data rows: i16 q4 record 2 write
 *  @data cols: u16 q0 record 1 */
function nested() {
  for (const [y, h] of rows) {
    for (const [x] of cols) {
      if (x > lim) break;
      plot(x, y + h, 65535);
    }
    rows.put(y + 1);
  }
}

/** @shiki self inputs: s
 *  @data cells: i16 q0 record 1 write */
function self() {
  for (const [p] of cells) {
    for (const [q] of cells) {
      cells.put(q + p * s);
    }
  }
}

/** @shiki keep inputs: d
 *  @data tri: i16 q0 record 3 write */
function keep() {
  for (const [a, b, c] of tri) {
    tri.put(a, b);
    plot(a, b, c + d);
  }
}

/** @shiki counted inputs: n, off
 *  @data marks: u16 q0 record 2 write */
function counted() {
  for (const [x, c] of marks.take(n)) {
    plot(x + off, 10, c);
    marks.put(x + 1, c);
  }
  for (const [x, c] of marks.take(2)) { plot(x, 20, c); }
}
`;

// Must not compile: [plan source, part of the error message].
const REFUSED = [
  [`/** @shiki r1 inputs: x
 *  @data d: i16 q8 record 1 */
function r1() { for (const [a] of d) { let cu = a; plot(x, x, cu); } }`, 'given a fraction'],
  [`/** @shiki r2 inputs: x
 *  @q a=4
 *  @data d: i16 q8 record 1 */
function r2() { for (const [a] of d) { plot(a, x, 1); } }`, 'region at Q8'],
  [`/** @shiki r3 inputs: x
 *  @data d: i16 q0 record 1 write */
function r3() { d.put(x); }`, 'outside a for...of'],
  [`/** @shiki r4 inputs: x
 *  @data d: i16 q0 record 1 */
function r4() { for (const [a] of d) { d.put(a); } }`, 'not declared write'],
  [`/** @shiki r5 inputs: x
 *  @data d: i16 q0 record 2 */
function r5() { plot(x, d[x], 1); }`, 'record 1'],
  [`/** @shiki r6 inputs: x
 *  @data d: i16 q0 record 1 */
function r6() { for (const [a] of d.take(1.5)) { plot(a, x, 1); } }`, 'Q0'],
  [`/** @shiki r7 inputs: x
 *  @data d: i16 q0 record 2 */
function r7() { for (const [a, b, c] of d) { plot(a, b, c); } }`, 'names for'],
  [`/** @shiki r8 inputs: x
 *  @data d: i16 q0 record 1 write */
function r8() { for (const [a] of d) { d.put(a, a); } }`, 'takes 1..1'],
];

// ---------------------------------------------------------------- random
let st = SEED >>> 0 || 1;
const rnd = () => { st ^= st << 13; st >>>= 0; st ^= st >>> 17; st ^= st << 5; st >>>= 0; return st / 4294967296; };
const ri = (lo, hi) => lo + Math.floor(rnd() * (hi - lo + 1));
const pick = a => a[Math.floor(rnd() * a.length)];
const ELEM = [
  () => pick([-32768, -1, 0, 1, 32767, ri(-300, 300), ri(-32768, 32767), ri(-120, 120) * 256, ri(0, 240), ri(0, 240), ri(0, 130)]),
  () => pick([0, 1, 32767, 32768, 65535, ri(0, 240), ri(0, 65535)]),
  () => pick([-(2 ** 31), 2 ** 31 - 1, 0, ri(-300, 300) * 65536 + ri(0, 65535), ri(-(2 ** 31), 2 ** 31 - 1)]),
];
const INPUT = () => pick([ri(0, 8), ri(0, 3), ri(-20, 260), ri(-5, 5) + pick([0, .5, .25]), ri(0, 70), 0, 1, 2, 3, 1e6, -1e6, ri(-4, 4) / 3]);

// shiki_fix's evaluation, drawn as the engine draws (as test_shiki_compile.mjs).
function expected(fn, input, args, mem) {
  const ctx = {segs: [], pen: {valid: false, x: 0, y: 0}, raster: 0};
  const px = v => { const p = Math.floor(v + .5); if (p < -480 || p > 720) throw new Fail('INVALID'); return p; };
  const add = seg => { const e = addSeg(ctx, seg); if (e) throw new Fail(e); };
  const colour = c => { if (c < 0 || c > 65535) throw new Fail('INVALID'); return c; };
  const at = (X, Y) => { ctx.pen.valid = true; ctx.pen.x = X; ctx.pen.y = Y; };
  const api = {
    move: (x, y) => { const X = px(x), Y = px(y); at(X, Y); },
    plot: (x, y, c) => { c = colour(c); const X = px(x), Y = px(y); add([X, Y, X, Y, c]); at(X, Y); },
    line: (x, y, c) => { c = colour(c); const X = px(x), Y = px(y); if (ctx.pen.valid) add([ctx.pen.x, ctx.pen.y, X, Y, c]); at(X, Y); },
  };
  try { fn(FIXM, api, Fail, input, args, mem); } catch (e) { if (!(e instanceof Fail)) throw e; return {status: e.status, seg: ctx.segs}; }
  return {status: 'DONE', seg: ctx.segs};
}

let bad = 0;
const rows = [];
const text = PLANS, q = qdecls(text);
for (const plan of findPlans(text, 'PLANS', ['shiki'])) {
  const qd = q.get(plan.name) ?? {};
  let C, fn;
  try {
    C = compileShiki(plan, qd);
    fn = emitFix(planAst(plan), plan.params, qd, plan.data);
  } catch (e) { bad++; rows.push(`${plan.name.padEnd(8)} NOT COMPILED: ${e.message}`); continue; }
  const n = {vectors: 0, same: 0, steps: 0, status: {}};
  for (let v = 0; v < VECTORS; v++) {
    const regions = plan.data.map(d => {
      const recs = pick([1, 2, 3, 5, 8, ri(1, 40)]);
      return makeRegion(d.type, d.q, d.k, Array.from({length: recs * d.k}, ELEM[d.type]));
    });
    const args = plan.params.map(() => ri(0, 65535));
    const input = plan.inputs.map(INPUT);
    const copies = regions.map(g => g.mem.slice());
    const h = register(C.bytes, args, regions);
    if (!h) { bad++; rows.push(`${plan.name}: the engine refused the compiled bytes`); break; }
    n.vectors++;
    const got = draw(h, input);
    if (got.status === 'LIMIT' && got.steps === 10000) { n.steps++; continue; }
    const want = expected(fn, input, args, copies);
    n.status[got.status] = (n.status[got.status] ?? 0) + 1;
    // Region contents are compared whatever the status: writes made before a
    // stop stay (sec.5.3.3).
    const memSame = regions.every((g, k) => g.mem.every((e, j) => e === copies[k][j]));
    const same = got.status === want.status && memSame &&
      (got.status !== 'DONE' || JSON.stringify(got.segs) === JSON.stringify(want.seg));
    if (same) { n.same++; continue; }
    if (++bad <= 5) console.log(`DIFF ${plan.name} input ${JSON.stringify(input)} args ${JSON.stringify(args)}\n` +
      `  engine ${got.status} ${JSON.stringify(got.segs).slice(0, 200)} mem ${regions.map(g => Array.from(g.mem).join(',')).join(' | ').slice(0, 200)}\n` +
      `  ast    ${want.status} ${JSON.stringify(want.seg).slice(0, 200)} mem ${copies.map(a => Array.from(a).join(',')).join(' | ').slice(0, 200)}`);
  }
  rows.push(`${plan.name.padEnd(8)} ${String(C.count).padStart(2)} instr | vectors ${n.vectors}, same ${n.same}` +
    `${n.steps ? `, step limit ${n.steps}` : ''} | ${Object.entries(n.status).map(([k, v]) => `${k} ${v}`).join(' ')}`);
}
for (const [src, want] of REFUSED) {
  const plan = findPlans(src, 'REFUSED', ['shiki'])[0], qd = qdecls(src).get(plan.name) ?? {};
  let msg = null;
  try { compileShiki(plan, qd); } catch (e) { msg = e.message; }
  if (msg === null || !msg.includes(want)) { bad++; rows.push(`${plan.name.padEnd(8)} REFUSAL WRONG: wanted "${want}", got ${msg === null ? 'a compiled plan' : `"${msg}"`}`); }
  else rows.push(`${plan.name.padEnd(8)} refused: ${msg}`);
}
console.log(rows.join('\n'));
console.log(bad ? `SHIKI DATA COMPILE FAIL (${bad})` : 'SHIKI DATA COMPILE PASS');
process.exit(bad ? 1 : 0);
