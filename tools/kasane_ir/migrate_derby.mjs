// Migration prototype: a copy of apps/derby with its plans as JS functions,
// the source form DERBY would keep once plans are compiled at build time
// (docs/kasane/js-to-ir.md section 5.3). Host only (Node).
//
//   node tools/kasane_ir/migrate_derby.mjs SRC_DIR PLANS.js OUT_DIR
//
// In derby_prog.js: the one-letter texts in T become the @plan functions of
// PLANS.js (inserted before T, which then names them), the T.map loop goes
// (its SILK lines are in the map function), prog('S0,0') becomes an empty
// plan, and prog() becomes the @planDecoder stub that lower_plans.mjs
// replaces. The result does not run unlowered: prog() throws.
import fs from 'node:fs';
import path from 'node:path';

const [src, plansFile, out] = process.argv.slice(2);
fs.mkdirSync(out, {recursive: true});
for (const f of fs.readdirSync(src)) if (fs.statSync(path.join(src, f)).isFile()) fs.copyFileSync(path.join(src, f), path.join(out, f));
const file = path.join(out, 'derby_prog.js');
let s = fs.readFileSync(file, 'utf8').replace(/\r/g, '');
const must = (re, what) => { if (!re.test(s)) throw new Error(`derby_prog.js: ${what} not found`); };
must(/^const OPS = .*\n/m, 'OPS');
s = s.replace(/^\/\/ ---- Programs as text.*\n/m, '')
  .replace(/^const OPS = .*\n/m, '')
  .replace(/^function prog\(src, arg\) \{\n(?:.*\n)*?\}\n/m,
    '/** @planDecoder */\nfunction prog(plan, arg) {\n  throw Error(\'plans are compiled at build time: tools/kasane_ir/lower_plans.mjs\');\n}\n');
must(/^const T = \{\n/m, 'T');
const plans = fs.readFileSync(plansFile, 'utf8').replace(/\r/g, '').replace(/^[^]*?'use strict';\n/, '');
const still = '\n// A plan with no drawing of its own: it carries typed points (gallop, HEAD ON).\n' +
  '/** @plan still inputs: */\nfunction still() {\n}\n';
s = s.replace(/^const T = \{\n(?:.*\n)*?\};\n/m, t => plans.trimEnd() + '\n' + still + t.replace(/^  (\w+): '[^']*'(,?)$/mg, '  $1$2'));
s = s.replace(/^for \(let k = 0; k < 8; \+\+k\) T\.map \+= .*\n/m, '').split("prog('S0,0')").join('prog(still)');
fs.writeFileSync(file, s);
console.log(`migrate_derby: ${file}`);
