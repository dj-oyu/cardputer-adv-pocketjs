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

class Fail extends Error { constructor(s) { super(s); this.status = s; } }
const F = Math.fround;
const MAX = 2n ** 31n - 1n, MIN = -(2n ** 31n);

// ------------------------------------------------ shiki-spec.md arithmetic
const fix = {sat: 0, satIn: 0};
const sat = (v, src = 'sat') => {          // v: BigInt
  if (v > MAX) { fix[src]++; return MAX; }
  if (v < MIN) { fix[src]++; return MIN; }
  return v;
};
const floorDiv = (v, s) => v >= 0n ? v >> s : -((-v + (1n << s) - 1n) >> s); // floor(v / 2^s)
// sec.2.3: v * 2^q, clamped to the int32 range in double, then rounded half
// away from zero.
const lround = (v, q) => {
  if (!Number.isFinite(v)) throw new Fail('INVALID');
  const x = Math.min(Math.max(v * 2 ** q, -(2 ** 31)), 2 ** 31 - 1);
  return sat(BigInt(Math.sign(x) * Math.floor(Math.abs(x) + .5)), 'satIn');
};
const SIN = Array.from({length: 257}, (_, k) => Math.round(Math.sin(k * Math.PI / 512) * 65536));
const TURN = 683565276n;                    // round(2^32 / 2pi): radians Q16 * TURN >> 32 = turns Q16
const X = {
  add: (a, b) => sat(a + b), sub: (a, b) => sat(a - b),
  mul: (a, b, s) => s >= 0 ? sat(floorDiv(a * b, BigInt(s))) : sat(a * b * (1n << BigInt(-s))),
  shift: (a, k) => k >= 0 ? sat(a << BigInt(k)) : floorDiv(a, BigInt(-k)),
  input: (v, q) => lround(v, q),
  sin: a => {                                // sec.4.4, a in Q16 turns
    const p = Number(BigInt.asUintN(16, a)), quad = p >> 14;
    let w = p & 0x3fff;
    if (quad & 1) w = 0x4000 - w;
    let v;
    if (w === 0x4000) v = SIN[256];
    else { const i = w >> 6, t = w & 63; v = SIN[i] + Math.floor((SIN[i + 1] - SIN[i]) * t / 64); }
    return BigInt(quad >= 2 ? -v : v);
  },
};
// The largest Q (<= 30) at which a literal factor fits an int32.
const litQ = v => { let q = 30; while (q > 0 && Math.abs(Math.round(v * 2 ** q)) > 2 ** 31 - 1) q--; return q; };

// Q0 names (sec.8.3 rule 1): counts, colours and integer draw arguments,
// the counters of for loops (plan_js: let v = 0, then the last statement of
// the repeat's body v += 1), and every name joined to one of those by a
// plain name-to-name assignment.
const INTS = {line: [2], plot: [2], cubic: [0, 1], linePattern: [2, 3, 4, 9]};
function q0Names(ast) {
  const out = new Set();
  const walk = list => list.forEach((x, k) => {
    if (x.k === 'repeat') {
      if (x.n.k === 'name') out.add(x.n.v);
      const last = x.body[x.body.length - 1], prev = list[k - 1];
      if (last?.k === 'assign' && last.o === '+=' && last.e.k === 'num' && +last.e.lit === 1 &&
          prev?.k === 'decl' && prev.d.length === 1 && prev.d[0].n === last.n && prev.d[0].e.k === 'num' && +prev.d[0].e.lit === 0)
        out.add(last.n);
      walk(x.body);
    }
    if (x.k === 'draw') for (const i of INTS[x.f] ?? []) if (x.args[i]?.k === 'name') out.add(x.args[i].v);
    if (x.k === 'decl') for (const d of x.d) if (d.e.k === 'name') links.push([d.n, d.e.v]);
    if (x.k === 'assign' && x.o === '=' && x.e.k === 'name') links.push([x.n, x.e.v]);
  });
  const links = [];
  walk(ast);
  // Names joined by a plain name-to-name assignment share the type: one
  // count or colour among them makes them all Q0 (const C0 = 0xffd9; cu = C0;
  // line(x, y, cu)).
  for (let grew = true; grew;) {
    grew = false;
    for (const [a, b] of links) if (out.has(a) !== out.has(b)) { out.add(a); out.add(b); grew = true; }
  }
  return out;
}

// The plan's AST as JS on BigInt raw values, every node computed at the Q
// its consumer asks for (sec.8.3).
function emitFix(ast, params, qdecl) {
  const ints = q0Names(ast);
  for (const [n, q] of Object.entries(qdecl)) if (ints.has(n) && q !== 0) throw new Error(`@q ${n}=${q}: ${n} is a count, colour or loop variable (Q0)`);
  // sec.8.3 rule 9: a Q0 name must not be given a value with a fraction (a
  // non-integer literal, a name that is not Q0, sin or a division), which Q0
  // would drop without a word.
  const Q = n => ints.has(n) ? 0 : qdecl[n] ?? 16;
  const fractional = e => e.k === 'num' ? !Number.isInteger(+e.lit) : e.k === 'name' ? Q(e.v) !== 0 :
    e.k === 'call' || e.k === '/' || (e.a && fractional(e.a)) || (e.b && fractional(e.b));
  const check = list => list.forEach(x => {
    if (x.k === 'decl') for (const d of x.d) if (ints.has(d.n) && fractional(d.e)) throw new Error(`${d.n} is Q0 (sec.8.3 rule 1) but is given a fraction`);
    if (x.k === 'assign' && ints.has(x.n) && fractional(x.e)) throw new Error(`${x.n} is Q0 (sec.8.3 rule 1) but is given a fraction`);
    if (x.k === 'repeat') {
      if (x.n.k === 'num' && !(Number.isInteger(+x.n.lit) && +x.n.lit >= 1 && +x.n.lit <= 255)) throw new Error(`REPEAT ${x.n.lit} outside 1..255`);
      check(x.body);
    }
  });
  check(ast);
  const consts = [];
  const C = v => { consts.push(v); return `__c[${consts.length - 1}]`; };
  // sec.8.3 rule 9: what sec.6.1 would refuse is a compile error, not a
  // silent saturation.
  const lit = (v, q) => { const r = Math.round(Math.abs(v) * 2 ** q); if (r > 2 ** 31 - 1) throw new Error(`literal ${v} does not fit Q${q}`); return lround(v, q); };
  const S = s => { if (s > 62) throw new Error(`MUL shift ${s} over 62`); if (s < -31) throw new Error(`MUL shift ${s} under -31`); return s; };
  const K = k => { if (Math.abs(k) > 31) throw new Error(`SHIFT ${k} outside -31..31`); return k; };
  const natural = e => e.k === 'name' ? Q(e.v) : e.k === 'num' ? null : 16;
  // A factor of *: [code, its Q].
  const factor = e => {
    if (e.k === 'name') return [e.v, Q(e.v)];
    if (e.k === 'num') { const q = litQ(+e.lit); return [C(lit(+e.lit, q)), q]; }
    return [g(e, 16), 16];
  };
  const g = (e, q) => {
    switch (e.k) {
      case 'num': return C(lit(+e.lit, q));
      case 'name': { const d = K(q - Q(e.v)); return d ? `$.shift(${e.v}, ${d})` : e.v; }
      case 'neg': return `$.sub(0n, ${g(e.a, q)})`;
      case '+': return `$.add(${g(e.a, q)}, ${g(e.b, q)})`;
      case '-': return `$.sub(${g(e.a, q)}, ${g(e.b, q)})`;
      case '*': { const [a, qa] = factor(e.a), [b, qb] = factor(e.b); return `$.mul(${a}, ${b}, ${S(qa + qb - q)})`; }
      case '/': {
        const c = +e.b.lit, k = Math.log2(Math.abs(c));
        if (Number.isInteger(k) && k >= 0) {
          let body;
          if (e.a.k === '*') { const [a, qa] = factor(e.a.a), [b, qb] = factor(e.a.b); body = `$.mul(${a}, ${b}, ${S(qa + qb - q + k)})`; }
          else body = `$.shift(${g(e.a, q)}, ${K(-k)})`;
          return c < 0 ? `$.sub(0n, ${body})` : body;
        }
        return g({k: '*', a: e.a, b: {k: 'num', lit: String(1 / c)}}, q);
      }
      case 'call': {
        const s = `$.sin($.mul(${g(e.args[0], 16)}, ${TURN}n, 32))`;
        return q === 16 ? s : `$.shift(${s}, ${K(q - 16)})`;
      }
    }
    throw new Error(e.k);
  };
  const s = list => list.map(x => {
    switch (x.k) {
      case 'input': return x.d.map(d => `const ${d.n} = $.input(__in[${d.k}] ?? 0, ${Q(d.n)});`).join('\n');
      case 'param': return `const ${x.n} = $.input(__args[${x.index}], ${Q(x.n)});`;
      case 'decl': return x.d.map(d => `let ${d.n} = ${g(d.e, Q(d.n))};`).join('\n');
      case 'assign': return `${x.n} = ${g(x.o === '=' ? x.e : {k: x.o[0], a: {k: 'name', v: x.n}, b: x.e}, Q(x.n))};`;
      case 'break': {
        const qa = natural(x.a), qb = natural(x.b), q = Math.max(qa ?? qb ?? 16, qb ?? qa ?? 16);
        return `if (${g(x.a, q)} > ${g(x.b, q)}) break;`;
      }
      case 'draw': {
        const ii = INTS[x.f] ?? [];
        return `__draw.${x.f}(${x.args.map((a, i) => ii.includes(i) ? `$.int(${g(a, 0)})` : `$.real(${g(a, 16)})`).join(', ')});`;
      }
      case 'repeat': {
        const reg = !(x.n.k === 'num' || (x.n.k === 'name' && params.includes(x.n.v)));
        return `{ const __n = Number(${g(x.n, 0)}); if (__n < ${reg ? 0 : 1} || __n > 255) throw new __Fail('INVALID');\n` +
          `for (let __k = 0; __k < __n; __k++) {\n${s(x.body)}\n} }`;
      }
    }
    throw new Error(x.k);
  }).join('\n');
  const fn = new Function('$', '__c', '__draw', '__Fail', '__in', '__args', s(ast));
  return (m, draw, Fl, input, args) => fn(m, consts, draw, Fl, input, args);
}
const FIXM = {...X, real: v => Number(v) / 65536, int: v => Number(v)};

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
// `@q name=bits, ...` in a plan's doc comment (sec.8.3).
const qdecls = text => {
  const out = new Map(), re = /@plan\s+([\w.$-]+)[^]*?\*\//g;
  let m;
  while ((m = re.exec(text))) {
    const q = /@q\s+([^\n*]*)/.exec(m[0]);
    out.set(m[1], Object.fromEntries((q?.[1] ?? '').split(',').map(t => t.trim().split('=')).filter(t => t[0]).map(([n, v]) => [n, Number(v)])));
  }
  return out;
};
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
