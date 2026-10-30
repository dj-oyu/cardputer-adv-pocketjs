// How far Q16.16 fixed point moves DERBY's plans from exact arithmetic,
// beside float32 (what the VM computes today). Host, Node only.
//
//   node tools/kasane_ir/q16_error.mjs PLANS.js CASES.json [CASES.json ...]
//
// CASES.json: [{plan, tag, args, inputs: [[8 numbers], ...]}] (derby_plans.mjs
// --cases, or q16_error.py's capture of the panning draws). Each vector runs
// the plan's @plan function three ways, from the same AST (plan_js.mjs):
//   f64  every operation in double: the reference.
//   f32  every operation rounded to float32 (Math.fround), as the VM.
//   q16  every value a signed 32-bit integer in units of 1/65536
//        (docs/kasane/shiki.md sec.5.3): + - saturate, * keeps the floor of
//        the exact product (an arithmetic right shift, as EE.VMUL.S16),
//        division by a power of two is a floor shift and by any other
//        constant a multiply by its rounded reciprocal (the compiler's rule),
//        sin is the exact sine rounded to the grid (an ideal table), inputs,
//        arguments and literals round to the nearest step.
// Every coordinate a plan hands to move/line/plot/linePattern/cubic is
// recorded before rounding to a pixel; the reports compare those with f64:
// the largest error in pixels, the 99.9th percentile, and how many endpoints
// land on a different pixel once both are rounded the same way (lround).
// Vectors whose status or number of coordinates differs between modes are
// counted, not compared. docs/kasane/shiki.md sec.5.5 has the results.
import fs from 'node:fs';
import path from 'node:path';
import {findPlans, planAst} from './plan_js.mjs';

// Q_FRAC (environment) changes the fraction bits, 16 by default.
const FRAC = Number(process.env.Q_FRAC ?? 16), ONE = 2 ** FRAC, MAX = 2 ** 31 - 1, MIN = -(2 ** 31);
const F = Math.fround;
function modes() {
  // sat: saturations in arithmetic; satIn: a literal or input already out of range.
  const q = {sat: 0, satIn: 0, peak: 0};
  const S = (v, src = 'sat') => {
    if (v > MAX) { q[src]++; v = MAX; } else if (v < MIN) { q[src]++; v = MIN; }
    const a = Math.abs(v);
    if (a > q.peak) q.peak = a;
    return v;
  };
  const Q = v => S(Math.round(v * ONE), 'satIn');
  // floor(a * b / ONE), exact: b = bh * ONE + bl, 0 <= bl < ONE, so both
  // products stay below 2^53 for FRAC <= 21.
  const mul = (a, b) => { const bh = Math.floor(b / ONE), bl = b - bh * ONE; return S(a * bh + Math.floor(a * bl / ONE)); };
  const pow2 = c => { const a = Math.abs(c); return a >= 1 && Number.isInteger(Math.log2(a)); };
  const bad = v => { if (!Number.isFinite(v)) throw new Fail('INVALID'); return v; };
  return {
    q16: {
      state: q, lit: Q, input: Q, add: (a, b) => S(a + b), sub: (a, b) => S(a - b), mul, neg: a => S(-a),
      div: (a, c) => pow2(c) ? S(Math.floor(a / Math.abs(c)) * Math.sign(c)) : mul(a, Q(1 / c)),
      sin: a => S(Math.round(Math.sin(a / ONE) * ONE)), real: a => a / ONE,
      count: n => n / ONE,
    },
    f32: {
      lit: F, input: F, add: (a, b) => bad(F(a + b)), sub: (a, b) => bad(F(a - b)), mul: (a, b) => bad(F(a * b)),
      neg: a => -a, div: (a, c) => bad(F(a / c)), sin: a => bad(F(Math.sin(a))), real: a => a, count: n => n,
    },
    f64: {
      lit: v => v, input: v => v, add: (a, b) => bad(a + b), sub: (a, b) => bad(a - b), mul: (a, b) => bad(a * b),
      neg: a => -a, div: (a, c) => bad(a / c), sin: a => bad(Math.sin(a)), real: a => a, count: n => n,
    },
  };
}
class Fail extends Error { constructor(s) { super(s); this.status = s; } }

// The plan's AST as JS calling the mode's operations ($.add, ...).
function emit(ast, params) {
  const e = x => {
    switch (x.k) {
      case 'num': return `$.lit(${x.lit})`;
      case 'name': return x.v;
      case 'neg': return `$.neg(${e(x.a)})`;
      case '+': return `$.add(${e(x.a)}, ${e(x.b)})`;
      case '-': return `$.sub(${e(x.a)}, ${e(x.b)})`;
      case '*': return `$.mul(${e(x.a)}, ${e(x.b)})`;
      case '/':
        if (x.b.k !== 'num') throw new Error('division by a non-constant');
        return `$.div(${e(x.a)}, ${x.b.lit})`;
      case 'call': return `$.sin(${e(x.args[0])})`;
    }
    throw new Error(x.k);
  };
  const s = list => list.map(x => {
    switch (x.k) {
      case 'input': return x.d.map(d => `const ${d.n} = $.input(__in[${d.k}] ?? 0);`).join('\n');
      case 'param': return `const ${x.n} = $.input(__args[${x.index}]);`;
      case 'decl': return x.d.map(d => `let ${d.n} = ${e(d.e)};`).join('\n');
      case 'assign': {
        const op = {'+': 'add', '-': 'sub', '*': 'mul'}[x.o[0]];
        return `${x.n} = ${x.o === '=' ? e(x.e) : `$.${op}(${x.n}, ${e(x.e)})`};`;
      }
      case 'break': return `if (${e(x.a)} > ${e(x.b)}) break;`;
      case 'draw': return `__draw.${x.f}(${x.args.map(a => `$.real(${e(a)})`).join(', ')});`;
      case 'repeat': {
        const reg = !(x.n.k === 'num' || (x.n.k === 'name' && params.includes(x.n.v)));
        return `{ const __n = $.count(${e(x.n)}); if (!Number.isInteger(__n) || __n < ${reg ? 0 : 1} || __n > 255) throw new __Fail('INVALID');\n` +
          `for (let __k = 0; __k < __n; __k++) {\n${s(x.body)}\n} }`;
      }
    }
    throw new Error(x.k);
  }).join('\n');
  return new Function('$', '__draw', '__Fail', '__in', '__args', s(ast));
}

// Coordinates in the order the plan hands them over; the VM's bounds.
function recorder() {
  const xy = [];
  const c = v => { if (!Number.isFinite(v) || v < -480 || v > 720) throw new Fail('INVALID'); xy.push(v); };
  return {
    xy,
    api: {
      move: (x, y) => { c(x); c(y); },
      line: (x, y) => { c(x); c(y); },
      plot: (x, y) => { c(x); c(y); },
      linePattern: (x, y) => { c(x); c(y); },
      cubic: (n, col, ...q) => q.forEach(c),
    },
  };
}
function run(fn, mode, input, args) {
  const r = recorder();
  try { fn(mode, r.api, Fail, input, args); } catch (e) { if (!(e instanceof Fail)) throw e; return {status: e.status, xy: r.xy}; }
  return {status: 'DONE', xy: r.xy};
}

const [plansFile, ...caseFiles] = process.argv.slice(2);
if (!plansFile || !caseFiles.length) throw new Error('usage: q16_error.mjs PLANS.js CASES.json...');
const plans = new Map(findPlans(fs.readFileSync(plansFile, 'utf8'), path.basename(plansFile)).map(p => [p.name, p]));
const M = modes();
const lround = v => Math.sign(v) * Math.floor(Math.abs(v) + .5);
const rows = new Map(), compiled = new Map();
for (const file of caseFiles) for (const c of JSON.parse(fs.readFileSync(file, 'utf8'))) {
  const plan = plans.get(c.plan);
  if (!plan) throw new Error(`no @plan ${c.plan}`);
  if (!compiled.has(plan)) compiled.set(plan, emit(planAst(plan), plan.params));
  const fn = compiled.get(plan);
  const key = `${c.plan} ${c.tag.replace(/\d+$/, '')}`;
  const row = rows.get(key) ?? {plan: c.plan, tag: c.tag.replace(/\d+$/, ''), vectors: 0, both: 0, status: {f32: 0, q16: 0},
    shape: {f32: 0, q16: 0}, coords: 0, err: {f32: [], q16: []}, px: {f32: 0, q16: 0}, sat: 0, satIn: 0, peak: 0, worst: null};
  rows.set(key, row);
  for (const input of c.inputs) {
    row.vectors++;
    const ref = run(fn, M.f64, input, c.args);
    M.q16.state.sat = 0; M.q16.state.satIn = 0;
    const got = {f32: run(fn, M.f32, input, c.args), q16: run(fn, M.q16, input, c.args)};
    row.sat += M.q16.state.sat; row.satIn += M.q16.state.satIn;
    row.peak = Math.max(row.peak, M.q16.state.peak);
    if (ref.status !== 'DONE') continue;
    let ok = true;
    for (const m of ['f32', 'q16']) {
      if (got[m].status !== 'DONE') { row.status[m]++; ok = false; } else if (got[m].xy.length !== ref.xy.length) { row.shape[m]++; ok = false; }
    }
    if (!ok) continue;
    row.both++;
    row.coords += ref.xy.length;
    for (const m of ['f32', 'q16']) for (let i = 0; i < ref.xy.length; i++) {
      const d = Math.abs(got[m].xy[i] - ref.xy[i]);
      row.err[m].push(d);
      if (lround(got[m].xy[i]) !== lround(ref.xy[i])) row.px[m]++;
      if (m === 'q16' && (!row.worst || d > row.worst.d)) row.worst = {d, input, args: c.args, i};
    }
  }
}
const pct = (a, p) => { if (!a.length) return 0; const s = Float64Array.from(a).sort(); return s[Math.min(s.length - 1, Math.floor(p * s.length))]; };
const max = a => a.reduce((m, v) => v > m ? v : m, 0);
const f = v => v === 0 ? '0' : v < 1e-3 ? v.toExponential(1) : v.toFixed(3);
console.log('plan    source  vectors compared coords | f32 max   p99.9  px-diff | q16 max   p99.9  px-diff | q16 status/shape diff, saturations in arithmetic / of literals and inputs');
console.log(`fixed point: Q${31 - FRAC}.${FRAC} (1/${ONE}, up to ${2 ** (31 - FRAC)})`);
for (const r of [...rows.values()].sort((a, b) => a.plan.localeCompare(b.plan) || a.tag.localeCompare(b.tag))) {
  console.log(`${r.plan.padEnd(7)} ${r.tag.padEnd(7)} ${String(r.vectors).padStart(7)} ${String(r.both).padStart(8)} ${String(r.coords).padStart(6)} | ` +
    `${f(max(r.err.f32)).padStart(7)} ${f(pct(r.err.f32, .999)).padStart(7)} ${String(r.px.f32).padStart(6)} | ` +
    `${f(max(r.err.q16)).padStart(7)} ${f(pct(r.err.q16, .999)).padStart(7)} ${String(r.px.q16).padStart(6)} | ` +
    `${r.status.q16}/${r.shape.q16} (f32 ${r.status.f32}/${r.shape.f32}), ${r.sat} / ${r.satIn}`);
}
const worst = [...rows.values()].filter(r => r.worst).sort((a, b) => b.worst.d - a.worst.d).slice(0, 3);
for (const r of worst) console.log(`worst q16 ${r.plan} ${r.tag}: ${f(r.worst.d)} px at coordinate ${r.worst.i}, input ${JSON.stringify(r.worst.input)} args ${JSON.stringify(r.worst.args)}`);
