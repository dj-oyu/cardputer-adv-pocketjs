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
import {register, draw} from './shiki_ref.mjs';

const argv = process.argv.slice(2);
const altFile = argv[0] === '--alt' ? (argv.shift(), argv.shift()) : null;
const [plansFile, ...caseFiles] = argv;
const load = file => {
  const text = fs.readFileSync(file, 'utf8'), q = qdecls(text);
  return new Map(findPlans(text, path.basename(file)).map(p => [p.name, {plan: p, q: q.get(p.name) ?? {}}]));
};
const plans = load(plansFile), alt = altFile ? load(altFile) : new Map();

// shiki_fix's evaluation, drawn as the engine draws.
function expected(fn, input, args) {
  const seg = [];
  let pen = null, raster = 0;
  const px = v => { const p = Math.floor(v + .5); if (p < -480 || p > 720) throw new Fail('INVALID'); return p; };
  const add = (x0, y0, x1, y1, c) => {
    if (seg.length === 1024) throw new Fail('LIMIT');
    const cost = Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0)) + 1;
    if (raster + cost > 8192) throw new Fail('LIMIT');
    raster += cost; seg.push([x0, y0, x1, y1, c]);
  };
  const colour = c => { if (c < 0 || c > 65535) throw new Fail('INVALID'); return c; };
  const api = {
    move: (x, y) => { const X = px(x), Y = px(y); pen = [X, Y]; },
    plot: (x, y, c) => { c = colour(c); const X = px(x), Y = px(y); add(X, Y, X, Y, c); pen = [X, Y]; },
    line: (x, y, c) => { c = colour(c); const X = px(x), Y = px(y); if (pen) add(pen[0], pen[1], X, Y, c); pen = [X, Y]; },
  };
  try { fn(FIXM, api, Fail, input, args); } catch (e) { if (!(e instanceof Fail)) throw e; return {status: e.status, seg}; }
  return {status: 'DONE', seg};
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
