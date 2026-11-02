// What Shiki's integer CUBIC and LINE_PATTERN (docs/kasane/shiki-spec.md
// sec.4.6, 4.7, v1.2) change in DERBY's frames against the float VM they
// replace. Host, Node only. Results: docs/kasane/shiki.md sec.5.10.
//
//   node tools/kasane_ir/prim_vs_float.mjs [--alt ALT.js] PLANS.js CASES.json...
//
// For each plan that draws cubic() or linePattern(): the float VM's frame
// (plan_js.mjs reference(), float32 as ksn_procedural.c computes it) and
// Shiki's (shiki_compile.mjs, then shiki_ref.mjs) from the same captured
// inputs. Frames whose status or entry count differ are counted; the rest
// are compared entry by entry: plain segments by their largest end-point
// difference in pixels, pattern lines' geometry the same way, and their
// parameter entries field by field (u0q and duq in 1/256 cell).
import fs from 'node:fs';
import path from 'node:path';
import {findPlans, reference} from './plan_js.mjs';
import {qdecls} from './shiki_q.mjs';
import {compileShiki} from './shiki_compile.mjs';
import {register, draw} from './shiki_ref.mjs';

const argv = process.argv.slice(2);
const altFile = argv[0] === '--alt' ? (argv.shift(), argv.shift()) : null;
const [plansFile, ...caseFiles] = argv;
const load = file => {
  const text = fs.readFileSync(file, 'utf8'), q = qdecls(text);
  return new Map(findPlans(text, path.basename(file)).map(p => [p.name, {plan: p, q: q.get(p.name) ?? {}, text: p.text}]));
};
const plans = load(plansFile), alt = altFile ? load(altFile) : new Map();
const cases = caseFiles.flatMap(f => JSON.parse(fs.readFileSync(f, 'utf8')));
for (const name of [...new Set(cases.map(c => c.plan))].sort()) {
  const P = plans.get(name);
  if (!P || !/\b(cubic|linePattern)\s*\(/.test(P.text)) continue;
  const S = alt.get(name) ?? P;
  const C = compileShiki(S.plan, S.q);
  const r = {frames: 0, status: 0, count: 0, compared: 0, entries: 0, segDiff: 0, segMax: 0, geoDiff: 0, geoMax: 0,
    par: 0, u0q: 0, u0qMax: 0, duq: 0, duqMax: 0, other: 0, bothFail: 0};
  for (const c of cases.filter(c => c.plan === name)) {
    const h = register(C.bytes, c.args);
    for (const input of c.inputs) {
      r.frames++;
      const old = reference(P.plan, input, c.args), now = draw(h, input);
      if (old.status !== 'DONE' && now.status !== 'DONE' && old.status === now.status) { r.bothFail++; continue; }
      if (old.status !== now.status) { r.status++; (r.why ??= {})[`${old.status}->${now.status}`] = (r.why[`${old.status}->${now.status}`] ?? 0) + 1; continue; }
      if (old.seg.length !== now.segs.length) { r.count++; continue; }
      r.compared++;
      // A pattern line is a geometry entry (x0 biased by 4096) and the
      // parameter entry after it; walk them in order to tell them apart.
      for (let k = 0; k < old.seg.length; k++) {
        const a = old.seg[k], b = now.segs[k];
        r.entries++;
        const d = Math.max(...[0, 1, 2, 3].map(i => Math.abs(a[i] - b[i])));
        if (a[0] >= 2048) {
          if (d || a[4] !== b[4]) { r.geoDiff++; r.geoMax = Math.max(r.geoMax, d); }
          const pa = old.seg[++k], pb = now.segs[k];
          r.entries++;
          if (pa.join() === pb.join()) continue;
          r.par++;
          const wrap = 256 * ((pa[1] >> 8) & 31);
          const dist = (x, y) => { const e = Math.abs(x - y) % wrap; return Math.min(e, wrap - e); };
          const du = dist(pa[2] & 0xffff, pb[2] & 0xffff), dd = dist(pa[3] & 0x7fff, pb[3] & 0x7fff);
          if (du) { r.u0q++; r.u0qMax = Math.max(r.u0qMax, du); }
          if (dd) { r.duq++; r.duqMax = Math.max(r.duqMax, dd); }
          if (pa[0] !== pb[0] || pa[1] !== pb[1] || pa[4] !== pb[4]) r.other++;
        } else if (d || a[4] !== b[4]) { r.segDiff++; r.segMax = Math.max(r.segMax, d); }
      }
    }
  }
  console.log(`${name.padEnd(7)} ${C.count} instr | frames ${r.frames}, both failed alike ${r.bothFail}, status differs ${r.status}${r.why ? ' ' + JSON.stringify(r.why) : ''}, entry count differs ${r.count}, compared ${r.compared} (${r.entries} entries)`);
  console.log(`        plain segments differ ${r.segDiff} (max ${r.segMax} px) | pattern geometry differs ${r.geoDiff} (max ${r.geoMax} px) | pattern parameters differ ${r.par}: u0q ${r.u0q} (max ${r.u0qMax}/256 cell), duq ${r.duq} (max ${r.duqMax}/256), other fields ${r.other}`);
}
