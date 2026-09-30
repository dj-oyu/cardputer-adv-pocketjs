// Build step prototype: an app's JS with its @plan functions replaced by
// compiled plans, so the shipped source keeps no plan function bodies.
// Host only (Node). docs/kasane/js-to-ir.md section 5.3.
//
//   node tools/kasane_ir/lower_plans.mjs [--ids APP] SRC_DIR OUT_DIR
//
// Copies every file of SRC_DIR to OUT_DIR. In each .js file:
//   /** @plan name inputs: ... */ function f(...) { ... }
//     -> const f = '<the compiled IR, nibble-packed (pack.mjs)>';
//     -> const f = 'APP.name';          with --ids (flash plans, not in the
//                                        firmware yet: js-to-ir.md section 4 B)
//   /** @planDecoder */ function prog(...) { ... }
//     -> pack.mjs's nibble decoder, under the same name (packed form only;
//        --ids drops it, register() would take the name)
// and writes OUT_DIR/plans.json (each plan's name, file, IR and size).
// A file without either marker is copied byte for byte. A plan outside the
// subset fails the build with the file, line and rule (plan_js.mjs RULES).
import fs from 'node:fs';
import path from 'node:path';
import {findPlans, compilePlan} from './plan_js.mjs';
import {encodeNibble, literal, DECODER_NIBBLE} from './pack.mjs';

export function lowerText(text, file, ids = null) {
  const plans = findPlans(text, file), report = [];
  let out = '', at = 0;
  for (const p of plans) {
    const c = compilePlan(p);
    if (c.count > 64) throw new Error(`${file}: ${p.name}: ${c.count} instructions (register() takes 64)`);
    const packed = encodeNibble(c.text);
    out += text.slice(at, p.start) + `const ${p.fn} = ${ids ? `'${ids}.${p.name}'` : literal(packed)};`;
    at = p.end;
    report.push({name: p.name, file, ir: c.text, instructions: c.count, packedBytes: packed.length,
      sourceBytes: p.end - p.start});
  }
  out += text.slice(at);
  const dec = /\/\*\*\s*@planDecoder\s*\*\/\s*function\s+([A-Za-z_$][\w$]*)\s*\([^)]*\)\s*\{[^]*?\n\}\n/.exec(out);
  if (dec) {
    const body = ids ? '' : DECODER_NIBBLE.replace('function prog(', `function ${dec[1]}(`);
    out = out.slice(0, dec.index) + body + out.slice(dec.index + dec[0].length);
  } else if (plans.length && !ids && !/@planDecoder/.test(text))
    ; // the decoder may live in another file of the app
  return {text: out, report, decoder: !!dec};
}

function main() {
  const args = process.argv.slice(2);
  let ids = null;
  if (args[0] === '--ids') { ids = args[1]; args.splice(0, 2); }
  const [src, dst] = args;
  fs.mkdirSync(dst, {recursive: true});
  const report = [];
  let decoders = 0;
  for (const f of fs.readdirSync(src)) {
    const from = path.join(src, f), to = path.join(dst, f);
    if (!fs.statSync(from).isFile()) continue;
    if (!/\.m?js$/.test(f)) { fs.copyFileSync(from, to); continue; }
    const text = fs.readFileSync(from, 'utf8');
    if (!/@plan/.test(text)) { fs.copyFileSync(from, to); continue; }
    const r = lowerText(text, f, ids);
    fs.writeFileSync(to, r.text);
    report.push(...r.report); decoders += r.decoder;
  }
  if (report.length && !ids && decoders !== 1) throw new Error(`${src}: ${decoders} @planDecoder functions (want 1)`);
  fs.writeFileSync(path.join(dst, 'plans.json'), JSON.stringify(report, null, 1));
  const sum = k => report.reduce((a, r) => a + r[k], 0);
  console.log(`lower_plans: ${report.length} plans, ${sum('instructions')} instructions; source ${sum('sourceBytes')} B of ` +
    `plan functions -> ${ids ? 'ids' : sum('packedBytes') + ' B packed'}`);
}
if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(new URL(import.meta.url).pathname.replace(/^\/(\w:)/, '$1'))) main();
