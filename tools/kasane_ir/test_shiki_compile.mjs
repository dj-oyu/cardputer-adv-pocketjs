// The @shiki compiler against the AST evaluation of the same rules (host,
// Node only).
//
//   node tools/kasane_ir/test_shiki_compile.mjs [--alt ALT.js] PLANS.js CASES.json [CASES.json ...]
//
// For every plan of PLANS.js (or of ALT.js where it has one of the same
// name, e.g. plans_q16/derby_pan.js's @q declarations) and every captured
// vector of CASES.json (q16_error.py's and check_equivalence.py's captures):
// compile it (shiki_compile.mjs), run the bytes through the host reference
// engine (shiki_ref.mjs, which equals the C engine bit for bit, shiki_diff.mjs
// and shiki_device.mjs), and run shiki_fix.mjs's evaluation of the AST with
// a recorder that makes segments as sec.4.5 and sec.4.8 say (pixel =
// floor(value + 0.5), the -480..720 range, the 1,024-segment and
// 8,192-raster limits). Status and every segment must agree. The step limit
// is the engine's alone (the AST has no steps), so a vector the engine stops
// on it is counted and skipped. A plan the compiler refuses (cubic(),
// linePattern(), a rule-9 error) is listed with the reason.
import fs from 'node:fs';
import path from 'node:path';
import {findPlans, planAst} from './plan_js.mjs';
import {qdecls} from './shiki_q.mjs';
import {Fail, FIXM, emitFix} from './shiki_fix.mjs';
import {compileShiki} from './shiki_compile.mjs';
import {register, draw, addSeg, cubicOp, patternOp} from './shiki_ref.mjs';

const argv = process.argv.slice(2);
const altFile = argv[0] === '--alt' ? (argv.shift(), argv.shift()) : null;
const [plansFile, ...caseFiles] = argv;
const load = file => {
  const text = fs.readFileSync(file, 'utf8'), q = qdecls(text);
  return new Map(findPlans(text, path.basename(file)).map(p => [p.name, {plan: p, q: q.get(p.name) ?? {}}]));
};
const plans = load(plansFile), alt = altFile ? load(altFile) : new Map();

// shiki_fix's evaluation, drawn as the engine draws. cubic() and
// linePattern() go through the reference's own procedures (sec.4.6, 4.7) on
// the raw Q16 values: what is checked there is that the compiler put the
// right values in the right registers, not the procedures themselves (those
// are shiki_diff.mjs's).
const RAW = v => Math.round(v * 65536);
function expected(fn, input, args) {
  const ctx = {segs: [], pen: {valid: false, x: 0, y: 0}, raster: 0};
  const px = v => { const p = Math.floor(v + .5); if (p < -480 || p > 720) throw new Fail('INVALID'); return p; };
  const add = seg => { const e = addSeg(ctx, seg); if (e) throw new Fail(e); };
  const colour = c => { if (c < 0 || c > 65535) throw new Fail('INVALID'); return c; };
  const at = (X, Y) => { ctx.pen.valid = true; ctx.pen.x = X; ctx.pen.y = Y; };
  const api = {
    move: (x, y) => { const X = px(x), Y = px(y); at(X, Y); },
    plot: (x, y, c) => { c = colour(c); const X = px(x), Y = px(y); add([X, Y, X, Y, c]); at(X, Y); },
    line: (x, y, c) => { c = colour(c); const X = px(x), Y = px(y); if (ctx.pen.valid) add([ctx.pen.x, ctx.pen.y, X, Y, c]); at(X, Y); },
    cubic: (n, c, ...P) => { const e = cubicOp(ctx, P.map(RAW), n, c); if (e) throw new Fail(e); },
    linePattern: (x, y, bits, cA, cB, u0, u1, w0, w1, N) => {
      const e = patternOp(ctx, RAW(x), RAW(y), [bits, cB, ...[u0, u1, w0, w1].map(RAW)], N, cA);
      if (e) throw new Fail(e);
    },
  };
  try { fn(FIXM, api, Fail, input, args); } catch (e) { if (!(e instanceof Fail)) throw e; return {status: e.status, seg: ctx.segs}; }
  return {status: 'DONE', seg: ctx.segs};
}

const cases = caseFiles.flatMap(f => JSON.parse(fs.readFileSync(f, 'utf8')));
const compiled = new Map();
let bad = 0;
const rows = [];
for (const name of [...new Set(cases.map(c => c.plan))].sort()) {
  const P = alt.get(name) ?? plans.get(name);
  if (!P) continue;
  let C, fn;
  try {
    C = compileShiki(P.plan, P.q);
    fn = emitFix(planAst(P.plan), P.plan.params, P.q);
  } catch (e) { rows.push(`${name.padEnd(8)} not compiled: ${e.message}`); continue; }
  compiled.set(name, C);
  const n = {vectors: 0, same: 0, steps: 0, status: {}};
  for (const c of cases.filter(c => c.plan === name)) {
    const h = register(C.bytes, c.args);
    if (!h) { bad++; rows.push(`${name}: the engine refused the compiled bytes (args ${JSON.stringify(c.args)})`); break; }
    for (const input of c.inputs) {
      n.vectors++;
      const got = draw(h, input), want = expected(fn, input, c.args);
      if (got.status === 'LIMIT' && got.steps === 10000) { n.steps++; continue; }
      n.status[got.status] = (n.status[got.status] ?? 0) + 1;
      const same = got.status === want.status &&
        (got.status !== 'DONE' || JSON.stringify(got.segs) === JSON.stringify(want.seg));
      if (same) { n.same++; continue; }
      if (++bad <= 5) console.log(`DIFF ${name} ${c.tag} input ${JSON.stringify(input)} args ${JSON.stringify(c.args)}\n  engine ${got.status} ${JSON.stringify(got.segs).slice(0, 300)}\n  ast    ${want.status} ${JSON.stringify(want.seg).slice(0, 300)}`);
    }
  }
  rows.push(`${name.padEnd(8)} ${String(C.count).padStart(2)} instr, inputs Q ${C.inputsQ.join(',') || '-'}${C.rematerialised.length ? `, remat ${C.rematerialised.length}` : ''} | ` +
    `vectors ${n.vectors}, same ${n.same}${n.steps ? `, step limit ${n.steps}` : ''} | ${Object.entries(n.status).map(([k, v]) => `${k} ${v}`).join(' ')}`);
}
console.log(rows.join('\n'));
console.log(bad ? `SHIKI COMPILE FAIL (${bad})` : 'SHIKI COMPILE PASS');
process.exit(bad ? 1 : 0);
