// DERBY's plan texts packed one byte per field (Latin-1, one char = one byte
// in QuickJS), with a decoder that replaces prog() and returns the same rows.
// The alternative to flash plans that leaves native untouched
// (docs/kasane/js-to-ir.md section 2.1). Host only (Node).
//
//   node tools/kasane_ir/pack.mjs IN.js OUT.js [--no-macro] [--nibble] [--check]
//
// rewrites derby_prog.js: T's texts (T.map with its SILK lines appended, as
// the file builds it) become packed literals, OPS/prog() become DECODER, and
// every plan is checked: the decoder's rows equal prog()'s for the same args.
// The app ships the nibble form: apps/derby/derby_prog_text.js is the source,
// apps/derby/derby_prog.js its output (--check: fail if OUT is not that).
//
// A field is one byte x: 0..199 the value x; 200..207 argument $0..$7;
// 210/211 an unsigned 16/24-bit value in the next 2/3 bytes; 212 minus the
// next field; 213 a decimal: length byte, then the digits. An op byte is the
// ksn_proc_op; 15 (the macro) is "INPUT k,k for k = 0..n-1", n the next field.
import fs from 'node:fs';
import vm from 'node:vm';
import {parseText, assemble} from './kir.mjs';

const FLD = ['14', '12', '123', '123', '12', '2', '', '23', '235', '235', '2', '23', '123', '123', '25'];
const FIELD = ['op', 'dst', 'a', 'b', 'value', 'color'];
export const DECODER = `const FLD = ['14', '12', '123', '123', '12', '2', '', '23', '235', '235', '2', '23', '123', '123', '25'];
function prog(s, a) {
  const c = [];
  let i = 0;
  // Not recursive: a closure that calls itself is a cycle, freed only by
  // the cycle collector, which the device runs near the heap limit.
  const v = () => {
    let g = 1, x;
    while ((x = s.charCodeAt(i++)) == 212) g = -g;
    if (x < 200) return g * x;
    if (x < 208) return g * a[x - 200];
    if (x == 213) { const l = s.charCodeAt(i); i += l + 1; return g * s.slice(i - l, i); }
    let n = 0;
    for (let k = x - 208; k--;) n = n * 256 + s.charCodeAt(i++);
    return g * n;
  };
  while (i < s.length) {
    const o = s.charCodeAt(i++);
    if (o == 15) { for (let k = 0, m = v(); k < m; ++k) c.push([1, k, k, 0, 0, 0]); continue; }
    const r = [o, 0, 0, 0, 0, 0], f = FLD[o];
    for (let j = 0; j < f.length; ++j) r[+f[j]] = v();
    c.push(r);
  }
  return c;
}
`;

function field(x, lit) {
  if (typeof x === 'object') return [200 + x.p];
  if (lit != null && !/^-?\d+$/.test(lit)) {
    const s = lit.startsWith('-') ? lit.slice(1) : lit;
    return (lit.startsWith('-') ? [212] : []).concat([213, s.length], [...s].map(ch => ch.charCodeAt(0)));
  }
  if (x < 0) return [212, ...field(-x)];
  if (x < 200) return [x];
  if (x < 65536) return [210, x >> 8, x & 255];
  if (x < 1 << 24) return [211, x >> 16, (x >> 8) & 255, x & 255];
  throw new RangeError(`field ${x}`);
}
export function encode(text, macro = true) {
  const code = parseText(text), out = [];
  let pc = 0;
  if (macro) {
    let n = 0;
    while (n < code.length && code[n].op === 1 && code[n].dst === n && code[n].a === n) n++;
    if (n > 1) { out.push(15, ...field(n)); pc = n; }
  }
  for (; pc < code.length; ++pc) {
    const i = code[pc];
    out.push(i.op);
    for (const k of FLD[i.op]) {
      const name = FIELD[+k];
      out.push(...(name in i.par ? field({p: i.par[name]}) : field(i[name], name === 'value' ? i.lit : null)));
    }
  }
  return String.fromCharCode(...out);
}
// The nibble form: the op in the low 4 bits of its byte and the first
// register field in the high 4; further register fields two to a byte (high,
// then low); other fields as above. 15 is the macro, its count the high nibble.
const NR = [1, 2, 3, 3, 2, 0, 0, 2, 2, 2, 1, 2, 3, 3, 0];
export const DECODER_NIBBLE = `const FLD = ['14', '12', '123', '123', '12', '2', '', '23', '235', '235', '2', '23', '123', '123', '25'],
  NR = [1, 2, 3, 3, 2, 0, 0, 2, 2, 2, 1, 2, 3, 3, 0];
function prog(s, a) {
  const c = [];
  let i = 0;
  // Not recursive: a closure that calls itself is a cycle, freed only by
  // the cycle collector, which the device runs near the heap limit.
  const v = () => {
    let g = 1, x;
    while ((x = s.charCodeAt(i++)) == 212) g = -g;
    if (x < 200) return g * x;
    if (x < 208) return g * a[x - 200];
    if (x == 213) { const l = s.charCodeAt(i); i += l + 1; return g * s.slice(i - l, i); }
    let n = 0;
    for (let k = x - 208; k--;) n = n * 256 + s.charCodeAt(i++);
    return g * n;
  };
  while (i < s.length) {
    let h = s.charCodeAt(i++);
    const o = h & 15, r = [o, 0, 0, 0, 0, 0], f = FLD[o];
    if (o == 15) { for (let k = 0; k < h >> 4; ++k) c.push([1, k, k, 0, 0, 0]); continue; }
    for (let j = 0; j < f.length; ++j)
      r[+f[j]] = j >= NR[o] ? v() : j == 0 ? h >> 4 : j & 1 ? (h = s.charCodeAt(i++)) >> 4 : h & 15;
    c.push(r);
  }
  return c;
}
`;
export function encodeNibble(text, macro = true) {
  const code = parseText(text), out = [];
  let pc = 0;
  if (macro) {
    let n = 0;
    while (n < code.length && n < 15 && code[n].op === 1 && code[n].dst === n && code[n].a === n) n++;
    if (n > 1) { out.push(15 | n << 4); pc = n; }
  }
  for (; pc < code.length; ++pc) {
    const i = code[pc], f = FLD[i.op], regs = f.slice(0, NR[i.op]).split('').map(k => i[FIELD[+k]]);
    if (regs.some(r => r > 15)) throw new RangeError('register or input over 15');
    out.push(i.op | (regs[0] ?? 0) << 4);
    for (let j = 1; j < regs.length; j += 2) out.push(regs[j] << 4 | (regs[j + 1] ?? 0));
    for (const k of f.slice(NR[i.op])) {
      const name = FIELD[+k];
      out.push(...(name in i.par ? field({p: i.par[name]}) : field(i[name], name === 'value' ? i.lit : null)));
    }
  }
  return String.fromCharCode(...out);
}
export function literal(s) {
  return "'" + [...s].map(ch => {
    const c = ch.charCodeAt(0);
    return c >= 32 && c < 127 && ch !== "'" && ch !== '\\' ? ch : '\\x' + c.toString(16).padStart(2, '0');
  }).join('') + "'";
}

// Replaces the text file's header line over OPS/prog() in the packed file.
const GENERATED = `// ---- GENERATED by tools/kasane_ir/pack.mjs from derby_prog_text.js (the
// plans as text): edit that file and rerun pack.mjs, never this one.
`;
function main() {
  const [inp, out] = process.argv.slice(2), macro = !process.argv.includes('--no-macro'),
    nib = process.argv.includes('--nibble'), dec = nib ? DECODER_NIBBLE : DECODER;
  const src = fs.readFileSync(inp, 'utf8');
  const ctx = {Math};
  vm.createContext(ctx);
  vm.runInContext(`const M = Math, PI = M.PI, sin = M.sin, rnd = M.round, mx = M.max;
const SILK = [0xffff, 0x8c71, 0xf800, 0x237f, 0xffe0, 0x07e0, 0xfd20, 0xf81f];\n` +
    src.replace("'use strict';", '') + '\nglobalThis.__T = T; globalThis.__prog = prog;\n', ctx);
  // The decoder exactly as it goes into the app, in a context of its own.
  const dctx = {};
  vm.createContext(dctx);
  vm.runInContext(dec + 'globalThis.__dec = prog;', dctx);
  const T = ctx.__T, args = [3, 5, 7, 11, 13, 17, 19, 23];
  let packed = 0, text = 0;
  const lines = [];
  for (const [name, t] of Object.entries(T)) {
    const p = nib ? encodeNibble(t, macro) : encode(t, macro);
    // The rows must be the text file's own prog()'s, and kir's.
    const want = JSON.stringify(ctx.__prog(t, args)), got = JSON.stringify(dctx.__dec(p, args));
    if (want !== got) throw new Error(`${name}: decoded rows differ\n${want}\n${got}`);
    if (JSON.stringify(assemble(parseText(t), args)) !== want) throw new Error(`${name}: kir disagrees with prog()`);
    packed += p.length; text += t.length;
    lines.push(`  ${name}: ${literal(p)}`);
  }
  let res = src.replace(/^\/\/ ---- Programs as text.*\n(?:\/\/.*\n)*/m, GENERATED).replace(/^const OPS = .*\n/m, '').replace(/^function prog\(src, arg\) \{\n(?:.*\n)*?\}\n/m, dec)
    .replace(/^const T = \{\n(?:.*\n)*?\};\n/m, 'const T = {\n' + lines.join(',\n') + '\n};\n')
    .replace(/^for \(let k = 0; k < 8; \+\+k\) T\.map \+= .*\n/m, '');
  if (process.argv.includes('--check')) {
    // run_derby.py: the shipped file must be what the text file packs to.
    if (!fs.existsSync(out) || fs.readFileSync(out, 'utf8') !== res) {
      console.error(`${out} is stale: node tools/kasane_ir/pack.mjs ${inp} ${out}${nib ? ' --nibble' : ''}`);
      process.exit(1);
    }
  } else fs.writeFileSync(out, res);
  console.log(`packed ${Object.keys(T).length} plans: ${text} chars -> ${packed} bytes (${nib ? 'nibble, ' : ''}${macro ? 'with' : 'no'} INPUT macro)`);
}
if (process.argv[1] && import.meta.url.endsWith(process.argv[1].replace(/\\/g, '/').split('/').pop())) main();
