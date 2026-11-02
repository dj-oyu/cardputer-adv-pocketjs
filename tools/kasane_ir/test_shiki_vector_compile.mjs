// The @shiki compiler's vector loops and EMIT (v2.0 draft sec.6.2, D7)
// against the AST evaluation of the same rules, through the C engine (host,
// Node and gcc).
//
//   node tools/kasane_ir/test_shiki_vector_compile.mjs [--vectors N] [--seed S]
//
// Each plan below is compiled (shiki_compile.mjs) and drawn by the C engine
// itself (main/ui/kasane/ksn_shiki.c through tools/kasane_contract/shiki_run.c,
// built here with ASan and UBSan): the host reference (shiki_ref.mjs) has no
// vector instructions, and the prototype's C model is what the spec takes as
// normative. shiki_fix.mjs evaluates the same AST on copies of the same region
// contents. Status, segments (when DONE) and every region's contents after the
// run must agree. The second list is plans the compiler must refuse, each with
// the message it must give.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {findPlans, planAst} from './plan_js.mjs';
import {qdecls} from './shiki_q.mjs';
import {Fail, FIXM, emitFix} from './shiki_fix.mjs';
import {compileShiki} from './shiki_compile.mjs';
import {addSeg} from './shiki_ref.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const argv = process.argv.slice(2);
const opt = (n, d) => { const i = argv.indexOf(n); return i < 0 ? d : Number(argv[i + 1]); };
const VECTORS = opt('--vectors', 200), SEED = opt('--seed', 1);

const PLANS = `
/** @shiki affine inputs: a, b, c, d, tx, ty
 *  @data pts: i16 q4 record 2
 *  @data out: i16 q4 record 2 write */
function affine() {
  // The affine transform of every point (the prototype's affine128).
  for (const [x, y] of pts.blocks()) {
    acc.set(tx); acc.mack(x, a); acc.mack(y, b);
    out.setLane(0, acc.out(14));
    acc.set(ty); acc.mack(x, c); acc.mack(y, d);
    out.setLane(1, acc.out(14));
  }
  points(out, 2016);
}

/** @shiki ops inputs: s, k, n, st
 *  @data v: i16 q0 record 3 write */
function ops() {
  for (const [x, y, z] of v.from(st).take(n).blocks()) {
    const m = vmin(x, y), M = vmax(x, y) - splat(s);
    v.setLane(2, m + M - z);
    v.setLane(0, iota() + splat(k * 2));
    acc.zero(); acc.mac(m, M); acc.mac(z, splat(3));
    const o = acc.out(4), p = acc.out(2);
    v.setLane(1, o - p);
  }
}

/** @shiki acc inputs: s
 *  @data w: i16 q0 record 1 write */
function acc2() {
  // The accumulator lives across blocks and loops: no zero here.
  for (const [x] of w.blocks()) { acc.mack(x, s); w.setLane(0, acc.out(1)); }
  for (const b of w.take(3).blocks()) { w.setLane(0, acc.out(0) + iota()); }
}

/** @shiki two inputs: g
 *  @data src: i16 q0 record 2
 *  @data dst: i16 q2 record 4 write */
function two(colour) {
  for (const [x] of src.blocks()) {
    const y = src.lane(1);
    dst.setLane(0, x); dst.setLane(1, y);
    dst.setLane(2, x + splat(g)); dst.setLane(3, (y - splat(g)) - splat(1));
  }
  lines(dst, colour);
  points(dst.from(1).take(2), 63);
}

/** @shiki emit inputs: s, n
 *  @data p: i16 q0 record 2 */
function emit() {
  points(p.from(s), 31);
  points(p.take(n), 2047);
  points(p.from(s).take(n), 65535);
  move(10, 10);
  line(20, 20, 1);
}

/** @shiki mix inputs: dx, n
 *  @data q: i16 q0 record 2 write */
function mix() {
  // Scalar loops around and next to a vector loop.
  for (let i = 0; i < n; i++) {
    for (const [x, y] of q.blocks()) { q.setLane(0, x + splat(dx + i)); q.setLane(1, vmax(y, splat(i))); }
  }
  for (const [x, y] of q) { plot(x, y, 1); }
}
`;

const REFUSED = [
  [`/** @shiki f1 inputs: s
 *  @data d: i16 q0 record 1 write */
function f1() { for (const [x] of d.blocks()) { const y = x * 2; d.setLane(0, y); } }`, '* on vectors'],
  [`/** @shiki f2 inputs: s
 *  @data d: i16 q0 record 1 write */
function f2() { for (const [x] of d.blocks()) { d.setLane(0, x + s); } }`, 'a scalar (use splat())'],
  [`/** @shiki f3 inputs: s
 *  @data d: i16 q0 record 1 write */
function f3() { for (const [x] of d.blocks()) { plot(s, s, 1); } }`, 'R7'],
  [`/** @shiki f4 inputs: s
 *  @data d: i16 q0 record 1 write */
function f4() { for (const [x] of d.blocks()) { for (const [y] of d.blocks()) { d.setLane(0, y); } } }`, 'R7'],
  [`/** @shiki f5 inputs: s
 *  @data d: i16 q0 record 1 write */
function f5() { acc.zero(); }`, 'not declared'],
  [`/** @shiki f6 inputs: s
 *  @data d: u16 q0 record 1 write */
function f6() { for (const [x] of d.blocks()) { d.setLane(0, x); } }`, 'i16'],
  [`/** @shiki f7 inputs: s
 *  @data d: i16 q0 record 1 */
function f7() { for (const [x] of d.blocks()) { d.setLane(0, x); } }`, 'not declared write'],
  [`/** @shiki f8 inputs: s
 *  @data d: i16 q0 record 1 write */
function f8() { for (const [x] of d.blocks()) { const a = x + splat(1), b = a + splat(2), c = b + splat(3), e = c + splat(4),
  f = e + splat(5), g = f + splat(6), h = g + splat(7), i = h + splat(8), j = i + splat(9);
  d.setLane(0, x + a + b + c + e + f + g + h + i + j); } }`, 'more than 8 vectors'],
  [`/** @shiki f9 inputs: s
 *  @data d: i16 q0 record 1 write */
function f9() { for (const [x] of d.blocks()) { d.setLane(0, acc.out(33)); } }`, 'shift is 0..32'],
  [`/** @shiki f10 inputs: s
 *  @data d: i16 q0 record 1 */
function f10() { points(d, 1); }`, '2 or more fields'],
  [`/** @shiki f11 inputs: s
 *  @data d: i16 q0 record 1 write */
function f11() { for (const [x] of d.take(40000).blocks()) { d.setLane(0, x); } }`, '0..32,767'],
  [`/** @shiki f12 inputs: s
 *  @data d: i16 q0 record 1 write */
function f12() { for (const [x] of d.from(s / 2).blocks()) { d.setLane(0, x); } }`, 'Q0'],
  [`/** @shiki f13 inputs: s
 *  @data d: i16 q0 record 1 write */
function f13() { for (const [x] of d.blocks()) { d.setLane(1, x); } }`, 'has 1 fields'],
  [`/** @shiki f14 inputs: s
 *  @data d: i16 q0 record 2 */
function f14(c) { points(d, c + 1); }`, 'constant or an argument'],
  [`/** @shiki f15 inputs: s
 *  @data d: i16 q0 record 1 write */
function f15() { for (const [x] of d.blocks()) { let y = x; d.setLane(0, y); } }`, 'R7'],
];

// ---------------------------------------------------------------- random
let st = SEED >>> 0 || 1;
const rnd = () => { st ^= st << 13; st >>>= 0; st ^= st >>> 17; st ^= st << 5; st >>>= 0; return st / 4294967296; };
const ri = (lo, hi) => lo + Math.floor(rnd() * (hi - lo + 1));
const pick = a => a[Math.floor(rnd() * a.length)];
const ELEM = [
  () => pick([-32768, -1, 0, 1, 32767, ri(-300, 300), ri(-32768, 32767), ri(0, 240) * 16, ri(0, 240), ri(0, 130), ri(-4000, 4000)]),
  () => pick([0, 1, 65535, ri(0, 240), ri(0, 65535)]),
  () => pick([0, ri(-300, 300) * 65536, ri(-(2 ** 31), 2 ** 31 - 1)]),
];
const INPUT = () => pick([ri(0, 8), ri(0, 3), ri(-20, 260), ri(-5, 5) + pick([0, .5]), 16384, -16384, ri(-16384, 16384), 0, 1, 1e6, -1e6]);

// shiki_fix's evaluation, drawn as the engine draws (as test_shiki_data_compile.mjs).
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
    seg: (x0, y0, x1, y1, c) => add([x0, y0, x1, y1, c]),
  };
  try { fn(FIXM, api, Fail, input, args, mem, 0); } catch (e) { if (!(e instanceof Fail)) throw e; return {status: e.status, seg: ctx.segs}; }
  return {status: 'DONE', seg: ctx.segs};
}

// The C engine, once for every case (shiki_run.c's text contract).
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'shiki-vec-'));
const runner = path.join(dir, 'shiki_run');
execFileSync('gcc', ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover',
  '-Imain/ui/kasane', 'main/ui/kasane/ksn_shiki.c', 'tools/kasane_contract/shiki_run.c', '-lm', '-o', runner], {cwd: ROOT, stdio: 'inherit'});
const hex = b => Buffer.from(b).toString('hex');

let bad = 0;
const rows = [], cases = [], meta = [];
const text = PLANS, q = qdecls(text);
for (const plan of findPlans(text, 'PLANS', ['shiki'])) {
  const qd = q.get(plan.name) ?? {};
  let C, fn;
  try {
    C = compileShiki(plan, qd);
    fn = emitFix(planAst(plan), plan.params, qd, plan.data);
  } catch (e) { bad++; rows.push(`${plan.name.padEnd(8)} NOT COMPILED: ${e.message}`); continue; }
  for (let v = 0; v < VECTORS; v++) {
    // Every region has at least the first one's records (VLOAD/VSTORE of
    // another region at the loop's lanes: registration's rule).
    const recs = pick([1, 2, 7, 8, 9, 16, 17, ri(1, 40)]);
    // Half the vectors keep elements and inputs small, so that coordinates
    // land on the screen and DONE is compared too.
    const small = rnd() < .5;
    const el = d => small && d.type === 0 ? () => ri(-20, 230) * 2 ** Math.min(d.q, 2) : ELEM[d.type];
    const regions = plan.data.map((d, k) => {
      const r = k ? recs + pick([0, 0, ri(0, 5)]) : recs;
      return {type: d.type, q: d.q, k: d.k, r, e: Array.from({length: r * d.k}, el(d))};
    });
    const args = plan.params.map(() => pick([ri(0, 65535), 2016]));
    const input = plan.inputs.map(() => small ? ri(0, 6) : INPUT());
    const data = regions.map(g => `${g.type} ${g.q} ${g.k} ${g.r} ${g.e.join(' ')}`).join(' ');
    cases.push(`CASE ${hex(C.bytes)} ARGS ${args.length} ${args.join(' ')} INPUTS ${input.length} ${input.join(' ')} DATA ${regions.length} ${data}`.replace(/  +/g, ' '));
    meta.push({plan: plan.name, fn, input, args, regions});
  }
  rows.push(`${plan.name.padEnd(8)} ${String(C.count).padStart(2)} instr`);
}
const file = path.join(dir, 'cases.txt');
fs.writeFileSync(file, cases.join('\n') + '\n');
const out = execFileSync(runner, [file], {maxBuffer: 1 << 30}).toString().trimEnd().split('\n');
const tally = {};
out.forEach((line, k) => {
  const m = meta[k], t = (tally[m.plan] ??= {same: 0, n: 0, status: {}, steps: 0});
  t.n++;
  if (line === 'REJECT') { if (++bad <= 5) console.log(`REJECT ${m.plan}`); return; }
  const w = line.split(' '), status = w[1], steps = +w[2];
  if (status === 'LIMIT' && steps === 10000) { t.steps++; return; }
  const s = w.indexOf('S'), ns = +w[s + 1], segs = [];
  for (let j = 0; j < ns; j++) segs.push(w.slice(s + 2 + 5 * j, s + 7 + 5 * j).map(Number));
  const d = w.indexOf('D', s + 2 + 5 * ns), mem = [];
  for (let j = 0, o = d + 2; j < m.regions.length; j++) {
    const g = m.regions[j], n = g.r * g.k;
    mem.push(w.slice(o + 1, o + 1 + n).map(Number)); o += 1 + n;
  }
  const Arr = [Int16Array, Uint16Array, Int32Array];
  const copies = m.regions.map(g => Arr[g.type].from(g.e));
  const want = expected(m.fn, m.input, m.args, copies);
  t.status[status] = (t.status[status] ?? 0) + 1;
  const memSame = copies.every((a, j) => a.every((e, i) => e === mem[j][i]));
  const same = status === want.status && memSame && (status !== 'DONE' || JSON.stringify(segs) === JSON.stringify(want.seg));
  if (same) { t.same++; return; }
  if (++bad <= 5) console.log(`DIFF ${m.plan} input ${JSON.stringify(m.input)} args ${JSON.stringify(m.args)}\n` +
    `  engine ${status} ${JSON.stringify(segs).slice(0, 200)} mem ${mem.map(a => a.join(',')).join(' | ').slice(0, 300)}\n` +
    `  ast    ${want.status} ${JSON.stringify(want.seg).slice(0, 200)} mem ${copies.map(a => Array.from(a).join(',')).join(' | ').slice(0, 300)}`);
});
for (let k = 0; k < rows.length; k++) {
  const name = rows[k].trim().split(' ')[0], t = tally[name];
  if (t) rows[k] += ` | cases ${t.n}, same ${t.same}${t.steps ? `, step limit ${t.steps}` : ''} | ` +
    Object.entries(t.status).map(([s, n]) => `${s} ${n}`).join(' ');
}
for (const [src, want] of REFUSED) {
  const plan = findPlans(src, 'REFUSED', ['shiki'])[0], qd = qdecls(src).get(plan.name) ?? {};
  let msg = null;
  try { compileShiki(plan, qd); } catch (e) { msg = e.message; }
  if (msg === null || !msg.includes(want)) { bad++; rows.push(`${plan.name.padEnd(8)} REFUSAL WRONG: wanted "${want}", got ${msg === null ? 'a compiled plan' : `"${msg}"`}`); }
  else rows.push(`${plan.name.padEnd(8)} refused: ${msg.slice(0, 160)}`);
}
fs.rmSync(dir, {recursive: true, force: true});
console.log(rows.join('\n'));
console.log(bad ? `SHIKI VECTOR COMPILE FAIL (${bad})` : 'SHIKI VECTOR COMPILE PASS');
process.exit(bad ? 1 : 0);
