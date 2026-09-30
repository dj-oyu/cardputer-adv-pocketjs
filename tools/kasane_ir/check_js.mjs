// The compiled plans against the JS functions they came from (host, Node).
//
//   node tools/kasane_ir/check_js.mjs PLANS.js CASES.json DUMP.txt
//
// CASES.json (derby_plans.mjs --cases) lists each case's plan, registration
// arguments and inputs; DUMP.txt (run_ir.c with RUN_IR_DUMP) is what the VM
// drew from the compiled IR for each of them, in the same order. For every
// vector the plan's JS runs through plan_js.mjs's reference twice: rounded
// to float32 per operation as the VM computes (must match exactly) and as
// written, in double (counted, not required: the VM is single precision).
import fs from 'node:fs';
import path from 'node:path';
import {findPlans, reference} from './plan_js.mjs';

const [plansFile, casesFile, dumpFile] = process.argv.slice(2);
const plans = new Map(findPlans(fs.readFileSync(plansFile, 'utf8'), path.basename(plansFile)).map(p => [p.name, p]));
const cases = JSON.parse(fs.readFileSync(casesFile, 'utf8'));
const dump = fs.readFileSync(dumpFile, 'utf8').trim().split('\n');
const STATUS = {1: 'DONE', 2: 'INVALID', 3: 'LIMIT'};
const key = r => r.status + (r.status === 'DONE' ? ' ' + r.raster + ' ' + r.seg.flat().join(' ') : '');
let row = 0, gameBad = 0, drawBad = 0;
const out = [];
for (const c of cases) {
  const plan = plans.get(c.plan);
  if (!plan) throw new Error(`no @plan ${c.plan} in ${plansFile}`);
  const n = {vectors: 0, f32: 0, plain: 0, done: 0}, diffs = new Map();
  for (const input of c.inputs) {
    const d = dump[row++].split(' ').map(Number), vm = {status: STATUS[d[0]], raster: d[2], seg: []};
    for (let k = 0; k < d[1]; ++k) vm.seg.push(d.slice(3 + 5 * k, 8 + 5 * k));
    const f = reference(plan, input, c.args), p = reference(plan, input, c.args, {f32: false});
    n.vectors++; n.done += vm.status === 'DONE';
    if (key(f) === key(vm)) n.f32++;
    else {
      const why = vm.status === f.status ? 'both DONE, segments differ' : `VM ${vm.status}, JS ${f.status}`;
      diffs.set(why, (diffs.get(why) ?? 0) + 1);
      if (c.tag.startsWith('game')) gameBad++;
      if (vm.status === 'DONE' && f.status === 'DONE') drawBad++;
    }
    if (key(p) === key(vm)) n.plain++;
  }
  out.push(`${c.plan.padEnd(7)} ${c.tag.padEnd(7)} vectors ${String(n.vectors).padStart(4)}  VM done ${String(n.done).padStart(4)}  ` +
    `float32 JS = VM ${String(n.f32).padStart(4)}  double JS = VM ${String(n.plain).padStart(4)}` +
    (diffs.size ? '  differ: ' + [...diffs].map(([k, v]) => `${k} x${v}`).join(', ') : ''));
}
if (row !== dump.length) throw new Error(`dump has ${dump.length} vectors, cases ${row}`);
console.log(out.join('\n'));
const ok = !gameBad && !drawBad;
console.log(ok ? 'JS REFERENCE PASS' : `JS REFERENCE FAIL (game vectors ${gameBad}, drawn but different ${drawBad})`);
process.exit(ok ? 0 : 1);
