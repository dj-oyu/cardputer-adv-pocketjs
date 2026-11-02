// The build step: an app's JS with its @shiki functions removed, so the
// shipped source keeps no program bodies; they ship as the firmware's
// built-in Shiki table (emit_shiki_rom.mjs) from the same source. Host only
// (Node 16 or later). docs/kasane/shiki-spec.md sec.8.
//
//   node tools/kasane_ir/lower_plans.mjs --rom APP SRC_DIR OUT_DIR
//   node tools/kasane_ir/lower_plans.mjs --rom APP --file SRC.js OUT.js
//
// In each .js file, every `/** @shiki name ... */ f(...) { ... }` (a method
// of an object literal) or function is removed, with a method's comma and the
// rest of its line, and an object literal left empty by that
// (`const T = {};`) with them. The decoder `/** @planDecoder rom */ function
// prog(...) {...}` (the marker that names the app as one whose programs are
// built in) is removed too. The // comment lines right above either marker
// (and one blank line above them) go with it: they describe code that no
// longer ships. The app registers 'APP.name'.
//
// The float VM's @plan functions and their packed form went with the VM
// (Shiki v2.0): a file that still has one is refused. SRC_DIR -> OUT_DIR
// copies every file (a file without a marker byte for byte) and picks --rom
// by itself, APP being SRC_DIR's base name; --file is the firmware build's
// form (main/CMakeLists.txt, one chunk per command; tools/make_app_chunks.py
// checks the decoder at configure time; OUT is always rewritten, as the
// build's timestamps expect).
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {findPlans} from './plan_js.mjs';

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

export function lowerText(text, file) {
  text = text.replace(/\r\n/g, '\n');
  if (findPlans(text, file).length)
    throw new Error(`${file}: @plan functions went with the float VM (Shiki v2.0); write them as @shiki`);
  const plans = findPlans(text, file, ['shiki']), report = [];
  let out = '', at = 0;
  for (const p of plans) {
    const [from] = withComments(text, p.start);
    const tail = /^[ \t]*,?[ \t]*\n?/.exec(text.slice(p.end))[0];
    out += text.slice(at, from);
    at = p.end + tail.length;
    report.push({name: p.name, file, shiki: true, sourceBytes: p.end - p.start});
  }
  out += text.slice(at);
  out = out.replace(/^(?:const|let) [A-Za-z_$][\w$]* = \{\s*\};\n/gm, '');
  const dec = /\/\*\*\s*@planDecoder(?:\s+rom)?\s*\*\/\s*function\s+([A-Za-z_$][\w$]*)\s*\([^)]*\)\s*\{[^]*?\n\}\n/.exec(out);
  if (dec) {
    if (!dec[0].includes('rom'))
      throw new Error(`${file}: the @planDecoder must be marked rom (the packed form went with the float VM)`);
    const [from, indent] = withComments(out, dec.index);
    out = out.slice(0, from) + indent + out.slice(dec.index + dec[0].length);
  }
  return {text: out, report, decoder: !!dec};
}

function writeIfChanged(file, text) {
  if (!fs.existsSync(file) || fs.readFileSync(file, 'utf8') !== text) fs.writeFileSync(file, text);
}

function main() {
  const args = process.argv.slice(2);
  if (args[0] === '--rom') args.splice(0, 2);
  if (args[0] === '--file') {
    const [, src, dst] = args;
    const text = fs.readFileSync(src, 'utf8');
    fs.mkdirSync(path.dirname(dst), {recursive: true});
    fs.writeFileSync(dst, /@plan|@shiki/.test(text) ? lowerText(text, path.basename(src)).text : text);
    return;
  }
  const [src, dst] = args;
  fs.mkdirSync(dst, {recursive: true});
  const report = [];
  let decoders = 0;
  for (const f of fs.readdirSync(src)) {
    const from = path.join(src, f), to = path.join(dst, f);
    if (!fs.statSync(from).isFile()) continue;
    if (!/\.m?js$/.test(f)) { fs.copyFileSync(from, to); continue; }
    const text = fs.readFileSync(from, 'utf8');
    if (!/@plan|@shiki/.test(text)) { fs.copyFileSync(from, to); continue; }
    const r = lowerText(text, f);
    writeIfChanged(to, r.text);
    report.push(...r.report); decoders += r.decoder;
  }
  if (report.length && decoders !== 1) throw new Error(`${src}: ${decoders} @planDecoder functions (want 1)`);
  const bytes = report.reduce((a, r) => a + r.sourceBytes, 0);
  console.log(`lower_plans: ${report.length} programs removed (${bytes} B of source)`);
}
if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) main();
