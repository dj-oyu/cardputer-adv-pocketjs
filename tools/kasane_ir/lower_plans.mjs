// The build step: an app's JS with its @plan functions replaced by compiled
// plans, so the shipped source keeps no plan function bodies. Host only
// (Node 16 or later). docs/kasane/js-to-ir.md section 5.
//
//   node tools/kasane_ir/lower_plans.mjs [--ids APP] SRC_DIR OUT_DIR
//   node tools/kasane_ir/lower_plans.mjs --file SRC.js OUT.js
//
// In each .js file:
//   /** @plan name inputs: ... */ function f(...) { ... }
//     -> const f = '<the compiled IR, nibble-packed (pack.mjs)>';
//     -> const f = 'APP.name';          with --ids (flash plans, not in the
//                                        firmware yet: js-to-ir.md section 4 B)
//   /** @plan name inputs: ... */ f(...) { ... }      (a method)
//     -> f: '<the same>'                the property of the object literal
//        (DERBY's form: a global const per plan raised the evaluation peak
//        by 1.5 KB, js-to-ir.md section 5.4)
//   /** @planDecoder */ function prog(...) { ... }
//     -> pack.mjs's nibble decoder, under the same name (packed form only;
//        --ids drops it, register() would take the name)
// The // comment lines right above either marker (and one blank line above
// them) go with it: they describe code that no longer ships.
// SRC_DIR -> OUT_DIR copies every file (a file without either marker byte
// for byte), checks that an app with plans has exactly one @planDecoder, and
// writes OUT_DIR/plans.json (each plan's name, file, IR and size). --file is
// the firmware build's form (main/CMakeLists.txt, one chunk per command;
// tools/make_app_chunks.py checks the decoder count at configure time; OUT
// is always rewritten, as the build's timestamps expect). A plan outside the
// subset fails with the file, line and rule (plan_js.mjs RULES).
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {findPlans, compilePlan} from './plan_js.mjs';
import {encodeNibble, literal, DECODER_NIBBLE} from './pack.mjs';

// Where the text replaced at `at` really starts (the // lines right above it
// and one blank line above those), and the indentation to put back.
function withComments(text, at) {
  let s = at, indent = '';
  const bol = text.lastIndexOf('\n', s - 1) + 1;
  if (/^[ \t]*$/.test(text.slice(bol, s))) { indent = text.slice(bol, s); s = bol; }
  while (s > 0) {
    const prev = text.lastIndexOf('\n', s - 2) + 1;
    if (!/^[ \t]*\/\//.test(text.slice(prev, s))) break;
    s = prev;
  }
  if (s >= 2 && text[s - 1] === '\n' && text[s - 2] === '\n') s--;
  return [s, indent];
}

export function lowerText(text, file, ids = null) {
  text = text.replace(/\r\n/g, '\n');
  const plans = findPlans(text, file), report = [];
  let out = '', at = 0;
  for (const p of plans) {
    const c = compilePlan(p);
    if (c.count > 64) throw new Error(`${file}: ${p.name}: ${c.count} instructions (register() takes 64)`);
    const packed = encodeNibble(c.text);
    const value = ids ? `'${ids}.${p.name}'` : literal(packed);
    const [from, indent] = withComments(text, p.start);
    out += text.slice(at, from) + indent + (p.method ? `${p.fn}: ${value}` : `const ${p.fn} = ${value};`);
    at = p.end;
    report.push({name: p.name, file, ir: c.text, instructions: c.count, packedBytes: packed.length,
      sourceBytes: p.end - p.start});
  }
  out += text.slice(at);
  const dec = /\/\*\*\s*@planDecoder\s*\*\/\s*function\s+([A-Za-z_$][\w$]*)\s*\([^)]*\)\s*\{[^]*?\n\}\n/.exec(out);
  if (dec) {
    const body = ids ? '' : DECODER_NIBBLE.replace('function prog(', `function ${dec[1]}(`);
    const [from, indent] = withComments(out, dec.index);
    out = out.slice(0, from) + indent + body + out.slice(dec.index + dec[0].length);
  }
  return {text: out, report, decoder: !!dec};
}

function writeIfChanged(file, text) {
  if (!fs.existsSync(file) || fs.readFileSync(file, 'utf8') !== text) fs.writeFileSync(file, text);
}

function main() {
  const args = process.argv.slice(2);
  if (args[0] === '--file') {
    const [, src, dst] = args;
    const text = fs.readFileSync(src, 'utf8');
    fs.mkdirSync(path.dirname(dst), {recursive: true});
    fs.writeFileSync(dst, /@plan/.test(text) ? lowerText(text, path.basename(src)).text : text);
    return;
  }
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
    writeIfChanged(to, r.text);
    report.push(...r.report); decoders += r.decoder;
  }
  if (report.length && !ids && decoders !== 1) throw new Error(`${src}: ${decoders} @planDecoder functions (want 1)`);
  fs.writeFileSync(path.join(dst, 'plans.json'), JSON.stringify(report, null, 1));
  const sum = k => report.reduce((a, r) => a + r[k], 0);
  console.log(`lower_plans: ${report.length} plans, ${sum('instructions')} instructions; source ${sum('sourceBytes')} B of ` +
    `plan functions -> ${ids ? 'ids' : sum('packedBytes') + ' B packed'}`);
}
if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) main();
