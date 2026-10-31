// How far Shiki's fixed point (docs/kasane/shiki-spec.md) moves DERBY's plans
// from exact arithmetic, beside float32 (what the VM computes today). Host,
// Node only.
//
//   node tools/kasane_ir/q16_error.mjs [--alt ALT.js] PLANS.js CASES.json [CASES.json ...]
//
// CASES.json: [{plan, tag, args, inputs: [[8 numbers], ...]}] (derby_plans.mjs
// --cases, or q16_error.py's capture of the panning draws). Each vector runs
// the plan's @plan function three ways, from the same AST (plan_js.mjs):
//   f64  every operation in double: the reference.
//   f32  every operation rounded to float32 (Math.fround), as the VM.
//   fix  shiki-spec.md's integer semantics as @shiki would compile the
//        function: every value an int32 at a Q format (sec.8.3: declared by
//        `@q name=bits` in the plan's doc, 16 if not; Q0 for counts, colours
//        and loop variables), + - saturate after aligning to the result's
//        format, * is MUL with s = qa + qb - q (64-bit product, floor, then
//        saturate; a power-of-two divisor folds into s), literal factors at
//        the largest Q that fits, sin through the sec.4.4 table, inputs and
//        arguments by sec.2.3 (lround, then saturate), coordinates read as
//        Q16 with the compiler's +0.5 (sec.4.5).
// --alt: plans of the same names in ALT.js (plans_q16/derby_pan.js: the same
// functions with @q declarations) run in fix instead; f64 and f32 still run
// the original. Its f64 is compared with the original's too (alt f64).
// Every coordinate a plan hands to move/line/plot/linePattern/cubic is
// recorded before rounding; the report compares those with f64: the largest
// error in pixels, the 99.9th percentile, and how many endpoints land on a
// different pixel (f64 and f32 by lround; fix by floor of the value plus the
// half, as the engine draws). Vectors whose status or number of coordinates
// differs between modes are counted, not compared. Results: shiki.md sec.5.5,
// 5.6, 5.6.1.
import fs from 'node:fs';
import path from 'node:path';
import {findPlans, planAst} from './plan_js.mjs';
import {qdecls} from './shiki_q.mjs';
import {Fail, FIXM, emitFix, fix} from './shiki_fix.mjs';

const F = Math.fround;

// ----------------------------------------------- float32 and double modes
function emitFloat(ast, params) {
  const e = x => {
    switch (x.k) {
      case 'num': return `$.lit(${x.lit})`;
      case 'name': return x.v;
      case 'neg': return `$.neg(${e(x.a)})`;
      case '+': return `$.add(${e(x.a)}, ${e(x.b)})`;
      case '-': return `$.sub(${e(x.a)}, ${e(x.b)})`;
      case '*': return `$.mul(${e(x.a)}, ${e(x.b)})`;
      case '/': return `$.div(${e(x.a)}, ${x.b.lit})`;
      case 'call': return `$.sin(${e(x.args[0])})`;
    }
    throw new Error(x.k);
  };
  const s = list => list.map(x => {
    switch (x.k) {
      case 'input': return x.d.map(d => `const ${d.n} = $.lit(__in[${d.k}] ?? 0);`).join('\n');
      case 'param': return `const ${x.n} = $.lit(__args[${x.index}]);`;
      case 'decl': return x.d.map(d => `let ${d.n} = ${e(d.e)};`).join('\n');
      case 'assign': {
        const op = {'+': 'add', '-': 'sub', '*': 'mul'}[x.o[0]];
        return `${x.n} = ${x.o === '=' ? e(x.e) : `$.${op}(${x.n}, ${e(x.e)})`};`;
      }
      case 'break': return `if (${e(x.a)} > ${e(x.b)}) break;`;
      case 'draw': return `__draw.${x.f}(${x.args.map(e).join(', ')});`;
      case 'repeat': {
        const reg = !(x.n.k === 'num' || (x.n.k === 'name' && params.includes(x.n.v)));
        return `{ const __n = ${e(x.n)}; if (!Number.isInteger(__n) || __n < ${reg ? 0 : 1} || __n > 255) throw new __Fail('INVALID');\n` +
          `for (let __k = 0; __k < __n; __k++) {\n${s(x.body)}\n} }`;
      }
    }
    throw new Error(x.k);
  }).join('\n');
  return new Function('$', '__draw', '__Fail', '__in', '__args', s(ast));
}
const bad = v => { if (!Number.isFinite(v)) throw new Fail('INVALID'); return v; };
const FLOAT = {
  f32: {lit: F, add: (a, b) => bad(F(a + b)), sub: (a, b) => bad(F(a - b)), mul: (a, b) => bad(F(a * b)),
    neg: a => -a, div: (a, c) => bad(F(a / c)), sin: a => bad(F(Math.sin(a)))},
  f64: {lit: v => v, add: (a, b) => bad(a + b), sub: (a, b) => bad(a - b), mul: (a, b) => bad(a * b),
    neg: a => -a, div: (a, c) => bad(a / c), sin: a => bad(Math.sin(a))},
};

// Coordinates in the order the plan hands them over; the VM's bounds.
// fixed: the engine's test (shiki-spec.md sec.4.5): the pixel the value
// lands on, floor(value + 0.5), is within -480..720.
function recorder(fixed) {
  const xy = [];
  const out = fixed ? v => { const p = Math.floor(v + .5); return p < -480 || p > 720; } : v => !Number.isFinite(v) || v < -480 || v > 720;
  const c = v => { if (out(v)) throw new Fail('INVALID'); xy.push(v); };
  return {xy, api: {
    move: (x, y) => { c(x); c(y); }, line: (x, y) => { c(x); c(y); }, plot: (x, y) => { c(x); c(y); },
    linePattern: (x, y) => { c(x); c(y); }, cubic: (n, col, ...q) => q.forEach(c),
  }};
}
function run(fn, mode, input, args) {
  const r = recorder(mode === FIXM);
  try { fn(mode, r.api, Fail, input, args); } catch (e) { if (!(e instanceof Fail)) throw e; return {status: e.status, xy: r.xy}; }
  return {status: 'DONE', xy: r.xy};
}

// ----------------------------------------------------------------- main
const argv = process.argv.slice(2);
const altFile = argv[0] === '--alt' ? (argv.shift(), argv.shift()) : null;
const [plansFile, ...caseFiles] = argv;
if (!plansFile || !caseFiles.length) throw new Error('usage: q16_error.mjs [--alt ALT.js] PLANS.js CASES.json...');
const load = file => {
  const text = fs.readFileSync(file, 'utf8'), q = qdecls(text);
  return new Map(findPlans(text, path.basename(file)).map(p => {
    const ast = planAst(p);
    // fix compiles when first run: a plan replaced by --alt is never compiled
    // here, so its rule-9 errors (which the alternative answers) do not stop
    // the run.
    let fixFn = null;
    return [p.name, {plan: p, float: emitFloat(ast, p.params), get fix() { return fixFn ??= emitFix(ast, p.params, q.get(p.name) ?? {}); }}];
  }));
};
const plans = load(plansFile), alt = altFile ? load(altFile) : new Map();
const lroundPx = v => Math.sign(v) * Math.floor(Math.abs(v) + .5);
const rows = new Map();
for (const file of caseFiles) for (const c of JSON.parse(fs.readFileSync(file, 'utf8'))) {
  const P = plans.get(c.plan);
  if (!P) throw new Error(`no @plan ${c.plan}`);
  const A = alt.get(c.plan), tag = c.tag.replace(/\d+$/, ''), key = `${c.plan} ${tag}`;
  const row = rows.get(key) ?? {plan: c.plan + (A ? '*' : ''), tag, vectors: 0, both: 0, status: {f32: 0, fix: 0},
    shape: {f32: 0, fix: 0}, coords: 0, err: {f32: [], fix: []}, px: {f32: 0, fix: 0}, sat: 0, satIn: 0, worst: null, altF64: 0};
  rows.set(key, row);
  for (const input of c.inputs) {
    row.vectors++;
    const ref = run(P.float, FLOAT.f64, input, c.args);
    fix.sat = 0; fix.satIn = 0;
    const got = {f32: run(P.float, FLOAT.f32, input, c.args), fix: run((A ?? P).fix, FIXM, input, c.args)};
    row.sat += fix.sat; row.satIn += fix.satIn;
    if (A) {
      const d = run(A.float, FLOAT.f64, input, c.args);
      if (ref.status === 'DONE' && d.status === 'DONE' && d.xy.length === ref.xy.length)
        for (let i = 0; i < d.xy.length; i++) row.altF64 = Math.max(row.altF64, Math.abs(d.xy[i] - ref.xy[i]));
    }
    if (ref.status !== 'DONE') continue;
    let ok = true;
    for (const m of ['f32', 'fix']) {
      if (got[m].status !== 'DONE') { row.status[m]++; ok = false; } else if (got[m].xy.length !== ref.xy.length) { row.shape[m]++; ok = false; }
    }
    if (!ok) continue;
    row.both++;
    row.coords += ref.xy.length;
    for (let i = 0; i < ref.xy.length; i++) {
      const want = lroundPx(ref.xy[i]);
      const df = Math.abs(got.f32.xy[i] - ref.xy[i]), dq = Math.abs(got.fix.xy[i] - ref.xy[i]);
      row.err.f32.push(df); row.err.fix.push(dq);
      if (lroundPx(got.f32.xy[i]) !== want) row.px.f32++;
      if (Math.floor(got.fix.xy[i] + .5) !== want) row.px.fix++;
      if (!row.worst || dq > row.worst.d) row.worst = {d: dq, input, args: c.args, i};
    }
  }
}
const pct = (a, p) => { if (!a.length) return 0; const s = Float64Array.from(a).sort(); return s[Math.min(s.length - 1, Math.floor(p * s.length))]; };
const max = a => a.reduce((m, v) => v > m ? v : m, 0);
const f = v => v === 0 ? '0' : v < 1e-3 ? v.toExponential(1) : v.toFixed(3);
console.log('fixed point: shiki-spec.md v1 (@q declared, Q16 otherwise)' + (altFile ? `; plan* from ${path.basename(altFile)}` : ''));
console.log('plan     source  vectors compared coords | f32 max   p99.9  px-diff | fix max   p99.9  px-diff | fix status/shape diff, saturations in arithmetic / on entry');
for (const r of [...rows.values()].sort((a, b) => a.plan.localeCompare(b.plan) || a.tag.localeCompare(b.tag))) {
  console.log(`${r.plan.padEnd(8)} ${r.tag.padEnd(7)} ${String(r.vectors).padStart(7)} ${String(r.both).padStart(8)} ${String(r.coords).padStart(6)} | ` +
    `${f(max(r.err.f32)).padStart(7)} ${f(pct(r.err.f32, .999)).padStart(7)} ${String(r.px.f32).padStart(6)} | ` +
    `${f(max(r.err.fix)).padStart(7)} ${f(pct(r.err.fix, .999)).padStart(7)} ${String(r.px.fix).padStart(6)} | ` +
    `${r.status.fix}/${r.shape.fix} (f32 ${r.status.f32}/${r.shape.f32}), ${r.sat} / ${r.satIn}` + (r.plan.endsWith('*') ? `, alt f64 ${f(r.altF64)}` : ''));
}
const worst = [...rows.values()].filter(r => r.worst).sort((a, b) => b.worst.d - a.worst.d).slice(0, 3);
for (const r of worst) console.log(`worst fix ${r.plan} ${r.tag}: ${f(r.worst.d)} px at coordinate ${r.worst.i}, input ${JSON.stringify(r.worst.input)} args ${JSON.stringify(r.worst.args)}`);
