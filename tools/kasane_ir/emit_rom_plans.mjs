// The firmware's built-in plan table: an app's @plan functions compiled to
// const C (docs/kasane/flash-plan.md). Host only (Node 16 or later).
//
//   node tools/kasane_ir/emit_rom_plans.mjs OUT.c APP=FILE.js [APP=FILE.js | APP=text:FILE.js ...]
//        [--symbol NAME] [--json OUT.json]
//
// Each plan becomes an entry named 'APP.name' (the id lower_plans.mjs --ids
// APP writes into the app): its instructions, a patch per field that holds a
// registration argument ($n), and params, the function's declared argument
// count (register('APP.name', args) must pass exactly that many).
//
// The rows are what the shipped app registers today, not a second reading of
// the IR: the plan's nibble-packed text (pack.mjs, as lower_plans.mjs ships
// it) goes through the decoder the app ships (DECODER_NIBBLE), with every
// argument 0, so a patched field holds 0 in the table. The same text through
// kir's assemble() must agree under test arguments (Object.is, so -0 and +0
// differ); a disagreement stops the build. A float is written as its exact
// hex-float, so the C compiler cannot round it differently from JS.
// OUT.c defines `const ksn_proc_rom_entry NAME[]` (default ksn_proc_rom_plans)
// and `const unsigned NAME_count`. Names are unique across the table.
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import {fileURLToPath} from 'node:url';
import {findPlans, compilePlan} from './plan_js.mjs';
import {encodeNibble, DECODER_NIBBLE} from './pack.mjs';
import {parseText, assemble} from './kir.mjs';

const FIELD = {a: 0, value: 1, color: 2};  // ksn_proc_field
const PARAMS = 8, CODE = 64;

const decoder = (() => {
  const ctx = {};
  vm.createContext(ctx);
  vm.runInContext(DECODER_NIBBLE + 'globalThis.__dec = prog;', ctx);
  return ctx.__dec;
})();

// The exact C spelling of a float32 (v is already one): 0x1.xxxxxxp+e.
export function hexFloat(v) {
  if (Math.fround(v) !== v && !Number.isNaN(v)) throw new RangeError(`${v} is not a float32`);
  if (!Number.isFinite(v)) throw new RangeError(`non-finite ${v}`);
  const b = new DataView(new ArrayBuffer(4));
  b.setFloat32(0, v);
  const bits = b.getUint32(0), sign = bits >>> 31 ? '-' : '', e = (bits >>> 23) & 255, m = bits & 0x7fffff;
  if (!e && !m) return sign + '0.0f';
  const frac = (m << 1).toString(16).padStart(6, '0');
  return e ? `${sign}0x1.${frac}p${e - 127 >= 0 ? '+' : ''}${e - 127}f` : `${sign}0x0.${frac}p-126f`;
}

export function romEntries(app, file) {
  if (!/^[a-z][a-z0-9_]*$/.test(app)) throw new Error(`${app}: an app id is [a-z][a-z0-9_]*`);
  const text = fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n');
  const out = [];
  for (const p of findPlans(text, path.basename(file))) {
    const c = compilePlan(p), code = parseText(c.text), params = p.params.length;
    if (code.length < 1 || code.length > CODE) throw new Error(`${app}.${p.name}: ${code.length} instructions`);
    if (params > PARAMS) throw new Error(`${app}.${p.name}: ${params} arguments (a plan takes ${PARAMS})`);
    const packed = encodeNibble(c.text), zero = new Array(PARAMS).fill(0);
    const rows = decoder(packed, zero);
    // The decoder's rows under test arguments against kir's reading of the
    // same text: they must be the same numbers, sign of zero included.
    const test = [3, 5, 7, 11, 13, 17, 19, 23];
    const got = decoder(packed, test), want = assemble(code, test);
    if (got.length !== code.length || want.some((r, i) => r.some((x, j) => !Object.is(Number(x), got[i][j]))))
      throw new Error(`${app}.${p.name}: the shipped decoder's rows differ from kir's`);
    const patch = [];
    code.forEach((i, pc) => {
      for (const [f, n] of Object.entries(i.par)) {
        if (!(f in FIELD)) throw new Error(`${app}.${p.name}: argument in field ${f}`);
        if (!(n >= 0 && n < params)) throw new Error(`${app}.${p.name}: $${n} but ${params} arguments`);
        patch.push({pc, field: FIELD[f], param: n});
      }
    });
    patch.sort((x, y) => x.pc - y.pc || x.field - y.field);
    out.push({name: `${app}.${p.name}`, sym: `${app}_${p.name}`, rows, patch, params, ir: c.text});
  }
  return out;
}

// An app's text templates instead of @plan functions (MEGADEMO,
// apps/kasane/megademo_plans.js, given as APP=text:FILE): the file defines
// T (name -> template) and prog(text, args) -> rows, the decoder the app
// shipped before its plans moved here; hx and M are given. Each template
// becomes 'APP.name'. Its arguments are the $n it names (params = the
// largest n + 1); a patch is a field that changes when one argument alone
// changes, and must be field a, value or color. Every argument at once
// must give the rows the patches give, or the build stops.
export function textEntries(app, file) {
  if (!/^[a-z][a-z0-9_]*$/.test(app)) throw new Error(`${app}: an app id is [a-z][a-z0-9_]*`);
  const ctx = {};
  vm.createContext(ctx);
  vm.runInContext('const M = Math, hx = (s, i) => { const c = s.charCodeAt(i); return c > 57 ? c - 87 : c - 48; };\n' +
    fs.readFileSync(file, 'utf8') + '\nglobalThis.__T = T; globalThis.__prog = prog;', ctx);
  const T = ctx.__T, prog = (t, a) => ctx.__prog(t, a).map(r => r.map(Number)), out = [];
  const SLOT = {2: FIELD.a, 4: FIELD.value, 5: FIELD.color};
  for (const name of Object.keys(T)) {
    const text = T[name], params = Math.max(0, ...[...text.matchAll(/\$(\d)/g)].map(m => +m[1] + 1));
    if (params > PARAMS) throw new Error(`${app}.${name}: ${params} arguments (a plan takes ${PARAMS})`);
    const zero = new Array(params).fill(0), rows = prog(text, zero);
    if (rows.length < 1 || rows.length > CODE) throw new Error(`${app}.${name}: ${rows.length} instructions`);
    const patch = [];
    for (let n = 0; n < params; ++n) {
      const a = zero.slice(); a[n] = 7;
      prog(text, a).forEach((r, pc) => r.forEach((v, j) => {
        if (Object.is(v, rows[pc][j])) return;
        if (!(j in SLOT)) throw new Error(`${app}.${name}: $${n} in slot ${j} of instruction ${pc}`);
        patch.push({pc, field: SLOT[j], param: n});
      }));
    }
    patch.sort((x, y) => x.pc - y.pc || x.field - y.field);
    // All arguments together, against the patches applied to the zero rows.
    const test = [3, 5, 7, 11, 13, 17, 19, 23].slice(0, params), got = prog(text, test),
      want = rows.map(r => r.slice());
    for (const p of patch) want[p.pc][p.field === FIELD.a ? 2 : p.field === FIELD.value ? 4 : 5] = test[p.param];
    if (got.some((r, i) => r.some((x, j) => !Object.is(x, j === 4 ? Math.fround(want[i][j]) : want[i][j]))))
      throw new Error(`${app}.${name}: its arguments do not reach the rows as patches`);
    out.push({name: `${app}.${name}`, sym: `${app}_${name}`, rows, patch, params, ir: text});
  }
  return out;
}

export function emitC(entries, symbol = 'ksn_proc_rom_plans', sources = []) {
  const seen = new Set();
  for (const e of entries) {
    if (seen.has(e.name)) throw new Error(`${e.name}: two plans of that name`);
    seen.add(e.name);
  }
  const NL = '\n';
  let c = `/* Generated by tools/kasane_ir/emit_rom_plans.mjs${sources.length ? ' from ' + sources.join(', ') : ''}.` + NL +
    ' * Do not edit: the @plan functions are the source (docs/kasane/flash-plan.md). */' + NL +
    '#include "ksn_proc_plan.h"' + NL;
  const table = [];
  for (const e of entries) {
    const rows = e.rows.map(r => {
      const [op, dst, a, b, value, color] = r.map(Number);
      return `    {${op},${dst},${a},${b},${hexFloat(Math.fround(value))},${color}}`;
    });
    c += `static const ksn_proc_inst rom_${e.sym}[${e.rows.length}] = {` + NL + rows.join(',' + NL) + NL + '};' + NL;
    if (e.patch.length)
      c += `static const ksn_proc_patch rom_${e.sym}_patch[${e.patch.length}] = {` +
        e.patch.map(p => `{${p.pc},${p.field},${p.param}}`).join(',') + '};' + NL;
    table.push(`    {"${e.name}", rom_${e.sym}, ${e.patch.length ? `rom_${e.sym}_patch` : 'NULL'}, ` +
      `${e.rows.length}, ${e.patch.length}, ${e.params}}`);
  }
  return c + `const ksn_proc_rom_entry ${symbol}[${Math.max(entries.length, 1)}] = {` + NL +
    (table.length ? table.join(',' + NL) : '    {0}') + NL + '};' + NL +
    `const unsigned ${symbol}_count = ${entries.length};` + NL;
}

function main() {
  const args = process.argv.slice(2), opt = k => { const i = args.indexOf(k); if (i < 0) return null; const v = args[i + 1]; args.splice(i, 2); return v; };
  const symbol = opt('--symbol') ?? 'ksn_proc_rom_plans', json = opt('--json');
  const [out, ...specs] = args;
  if (!out) throw new Error('usage: emit_rom_plans.mjs OUT.c APP=FILE.js ...');
  const entries = [], sources = [];
  for (const s of specs) {
    const at = s.indexOf('=');
    if (at < 1) throw new Error(`${s}: expected APP=FILE.js`);
    const app = s.slice(0, at), src = s.slice(at + 1), text = src.startsWith('text:');
    entries.push(...(text ? textEntries(app, src.slice(5)) : romEntries(app, src)));
    sources.push(path.basename(text ? src.slice(5) : src));
  }
  const c = emitC(entries, symbol, sources);
  if (!fs.existsSync(out) || fs.readFileSync(out, 'utf8') !== c) fs.writeFileSync(out, c);
  if (json) fs.writeFileSync(json, JSON.stringify(entries.map(e => ({name: e.name, params: e.params,
    count: e.rows.length, patches: e.patch.length, ir: e.ir})), null, 1));
  const n = entries.reduce((a, e) => a + e.rows.length, 0), p = entries.reduce((a, e) => a + e.patch.length, 0);
  console.log(`emit_rom_plans: ${entries.length} plans, ${n} instructions, ${p} patches -> ${out}`);
}
if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) main();
