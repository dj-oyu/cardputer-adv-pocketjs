// Shiki v1's C engine against the host reference, on random programs
// (docs/kasane/shiki-spec.md sec.10.1). Host: Node and gcc.
//
//   node tools/kasane_ir/shiki_diff.mjs [--cases N] [--seed S] [--keep DIR]
//
// Builds tools/kasane_contract/shiki_run.c with main/ui/kasane/ksn_shiki.c
// (ASan/UBSan), writes N random cases in the shared text contract, runs them
// through the C runner and tools/kasane_ir/shiki_ref.mjs, and compares the
// output lines byte for byte, plus the two sine tables. The two engines were
// written independently from the spec; a difference is a bug in one of them
// or a hole in the spec. The encoder here is this file's own (not the
// reference's assemble()), so a shared encoding mistake cannot hide.
//
// The generator aims at the edges the spec defines: saturation, floor of
// negative products and shifts, .H operand saturation, every SIN quadrant
// boundary, coordinates at -480/720 and their halves, the 10,000-step,
// 1,024-segment and 8,192-raster limits, REPEAT_REG out of range, breaks out
// of nested loops, argument patches, lround halves at the JS boundary. A
// fifth of the programs are mutated byte for byte so registration's
// accept/reject is compared too.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {execFileSync} from 'node:child_process';

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../..');
const opt = (name, d) => { const i = process.argv.indexOf(name); return i < 0 ? d : process.argv[i + 1]; };
const N = Number(opt('--cases', 20000)), SEED = Number(opt('--seed', 1));
const dir = opt('--keep', fs.mkdtempSync(path.join(os.tmpdir(), 'shiki-diff-')));
fs.mkdirSync(dir, {recursive: true});

// ---------------------------------------------------------------- random
let st = SEED >>> 0 || 1;
const rnd = () => { st ^= st << 13; st >>>= 0; st ^= st >>> 17; st ^= st << 5; st >>>= 0; return st / 4294967296; };
const ri = (lo, hi) => lo + Math.floor(rnd() * (hi - lo + 1));
const pick = a => a[Math.floor(rnd() * a.length)];
const I32 = [0, 1, -1, 2, -2, 32767, 32768, -32768, -32769, 65535, 65536, -65536, 2 ** 31 - 1, -(2 ** 31),
  2 ** 30, -(2 ** 30), 0x4000, 0x8000, 0xC000, 0xFFFF, 0x10000, -0x4000, 46341, 0x7FFFFFFF - 5];
const imm32 = () => rnd() < .5 ? pick(I32) : rnd() < .5 ? ri(-70000, 70000) : ri(-(2 ** 31), 2 ** 31 - 1);
// A Q16 coordinate near the interesting pixels, with odd fractions.
const coordRaw = () => {
  const px = pick([-481, -480, -479, 0, 1, 119, 120, 239, 240, 719, 720, 721, ri(-500, 740), ri(0, 239)]);
  return px * 65536 + pick([0, 1, 32767, 32768, 32769, 65535, ri(0, 65535)]);
};

// ------------------------------------------------------------- encoder
const OP = {SET: 0, INPUT: 1, ADD: 2, MUL: 3, SIN: 4, REPEAT: 5, END: 6, MOVE: 7, PLOT: 8, LINE: 9, REPEAT_REG: 10,
  BREAK_IF_GT: 11, PLOT_C: 12, LINE_C: 13, CUBIC: 14, LINE_PATTERN: 15, SUB: 16, NEG: 17, SHIFT: 18,
  ADD_H: 19, SUB_H: 20, MUL_H: 21, SHIFT_H: 22, EACH: 23, FETCH: 24, PUT: 25, LOOKUP: 26};
function encode({inputs = [], args = [], patches = [], code}) {
  const head = 12 + inputs.length + args.length + 4 * patches.length, body = (head + 3) & ~3;
  const b = new Uint8Array(body + 12 * code.length);
  b.set([0x53, 0x4b, 0x49, 0x31, 1, 0, code.length, inputs.length, args.length, 0, patches.length, 0]);
  let o = 12;
  for (const q of inputs) b[o++] = q;
  for (const q of args) b[o++] = q;
  for (const p of patches) { b.set([p.pc, p.arg, p.field, 0], o); o += 4; }
  const dv = new DataView(b.buffer);
  code.forEach((i, k) => {
    const at = body + 12 * k;
    b.set([i.op, i.dst ?? 0, i.a ?? 0, i.b ?? 0], at);
    dv.setInt32(at + 4, i.imm ?? 0, true);
    dv.setUint16(at + 8, i.aux ?? 0, true);
  });
  return b;
}

// ----------------------------------------------------------- generator
const reg = () => ri(0, 15);
function program() {
  const nin = ri(0, 8), inputs = Array.from({length: nin}, () => pick([16, 16, 0, 24, 30, ri(0, 30)]));
  const code = [], open = [];
  const target = ri(4, 64);
  const heavy = rnd() < .15;                       // aim at the limits
  while (code.length < target) {
    const room = target - code.length - open.length; // keep room for the ENDs
    if (room <= 0) break;
    const r = rnd();
    if (r < .10 && open.length < 8 && room > 2) {
      if (rnd() < .7) code.push({op: OP.REPEAT, imm: heavy ? ri(100, 255) : pick([1, 2, 3, 5, 255, ri(1, 9)])});
      else code.push({op: OP.REPEAT_REG, a: reg()});
      open.push(code.length - 1);
    } else if (r < .16 && open.length) { code.push({op: OP.END}); open.pop(); }
    else if (r < .19 && open.length) code.push({op: OP.BREAK_IF_GT, a: reg(), b: reg()});
    else if (r < .30) code.push({op: OP.SET, dst: reg(), imm: rnd() < .4 ? coordRaw() : rnd() < .2 ? ri(0, 300) : imm32()});
    else if (r < .35 && nin) code.push({op: OP.INPUT, dst: reg(), a: ri(0, nin - 1)});
    else if (r < .55) {
      const op = pick([OP.ADD, OP.SUB, OP.NEG, OP.MUL, OP.MUL, OP.SHIFT, OP.ADD_H, OP.SUB_H, OP.MUL_H, OP.SHIFT_H, OP.SIN]);
      const i = {op, dst: reg(), a: reg()};
      if ([OP.ADD, OP.SUB, OP.MUL, OP.ADD_H, OP.SUB_H, OP.MUL_H].includes(op)) i.b = reg();
      if (op === OP.MUL) i.aux = pick([0, 1, 15, 16, 17, 31, 32, 48, 62, ri(0, 62)]);
      if (op === OP.MUL_H) i.aux = pick([0, 1, 14, 15, 16, 31, ri(0, 31)]);
      if (op === OP.SHIFT) i.imm = pick([-31, -16, -1, 0, 1, 16, 31, ri(-31, 31)]);
      if (op === OP.SHIFT_H) i.imm = pick([-15, -1, 0, 1, 14, ri(-15, 14)]);
      code.push(i);
    } else {
      const op = pick([OP.MOVE, OP.PLOT, OP.LINE, OP.LINE, OP.PLOT_C, OP.LINE_C]);
      const i = {op, a: reg(), b: reg()};
      if (op === OP.PLOT || op === OP.LINE) i.aux = ri(0, 65535);
      if (op === OP.PLOT_C || op === OP.LINE_C) i.dst = reg();
      code.push(i);
    }
  }
  while (open.length) { code.push({op: OP.END}); open.pop(); }
  if (code.length > 64) code.length = 64;          // may unbalance: a reject to compare
  // Arguments patched into SET's imm (any Q), PLOT/LINE's aux and REPEAT's
  // imm (Q0), sorted by (pc, field).
  const args = [], argv = [], patches = [];
  for (let k = 0; k < ri(0, 3); k++) {
    const pc = ri(0, code.length - 1), i = code[pc];
    let field, q, v;
    if (i.op === OP.SET) { field = 0; q = pick([16, 0, 24, ri(0, 30)]); v = rnd() < .5 ? ri(-500, 740) + pick([0, .5, -.5, .25, 1 / 65536]) : (rnd() - .5) * 1e5; }
    else if (i.op === OP.PLOT || i.op === OP.LINE) { field = 1; q = rnd() < .9 ? 0 : 16; v = rnd() < .9 ? ri(0, 65535) : pick([-1, 65536, 1.5]); }
    else if (i.op === OP.REPEAT) { field = 0; q = rnd() < .9 ? 0 : 16; v = pick([1, 2, 255, 0, 256, ri(1, 9)]); }
    else continue;
    if (patches.some(p => p.pc === pc && p.field === field)) continue;
    patches.push({pc, arg: args.length, field}); args.push(q); argv.push(v);
  }
  patches.sort((x, y) => x.pc - y.pc || x.field - y.field);
  const bytes = encode({inputs, args, patches, code});
  if (rnd() < .2) {                                // mutation: compare accept/reject
    const k = ri(0, bytes.length - 1);
    bytes[k] = rnd() < .5 ? ri(0, 255) : bytes[k] ^ (1 << ri(0, 7));
  }
  const inv = Array.from({length: rnd() < .1 ? ri(0, 10) : nin}, () => {
    const q = 16;
    return pick([ri(-600, 800), ri(-600, 800) + .5, -(ri(0, 600) + .5), (ri(-1e6, 1e6)) / 2 ** q, rnd() * 1e3 - 500,
      1e12, -1e12, 0, -0, ...(rnd() < .02 ? [NaN, Infinity] : [])]);
  });
  return {bytes, argv, inv};
}

// --------------------------------------------------------------- run
const hex = b => Buffer.from(b).toString('hex');
const lines = [];
for (let n = 0; n < N; n++) {
  const {bytes, argv, inv} = program();
  lines.push(`CASE ${hex(bytes)} ARGS ${argv.length} ${argv.map(String).join(' ')} INPUTS ${inv.length} ${inv.map(String).join(' ')}`.replace(/  +/g, ' '));
}
const cases = path.join(dir, 'cases.txt');
fs.writeFileSync(cases, lines.join('\n') + '\n');
const runner = path.join(dir, 'shiki_run');
execFileSync('gcc', ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover',
  '-Imain/ui/kasane', 'main/ui/kasane/ksn_shiki.c', 'tools/kasane_contract/shiki_run.c', '-lm', '-o', runner], {cwd: ROOT, stdio: 'inherit'});
const big = {maxBuffer: 1 << 30, cwd: ROOT};
const c = execFileSync(runner, [cases], big).toString().trimEnd().split('\n');
const js = execFileSync('node', [path.join(ROOT, 'tools/kasane_ir/shiki_ref.mjs'), cases], big).toString().trimEnd().split('\n');
const cSin = execFileSync(runner, [], {...big, env: {...process.env, SHIKI_SIN_TABLE: '1'}}).toString().trim();
const {SIN_TABLE} = await import(path.join(ROOT, 'tools/kasane_ir/shiki_ref.mjs'));
let bad = 0;
if (cSin !== Array.from(SIN_TABLE).join(' ')) { bad++; console.log('SIN TABLE DIFFERS'); }
if (c.length !== N || js.length !== N) { console.log(`line counts: C ${c.length}, JS ${js.length}, cases ${N}`); process.exit(1); }
const tally = {};
for (let k = 0; k < N; k++) {
  const key = c[k].startsWith('REJECT') ? 'REJECT' : c[k].split(' ')[1];
  tally[key] = (tally[key] ?? 0) + 1;
  if (c[k] !== js[k]) {
    if (++bad <= 5) console.log(`DIFF case ${k}\n  ${lines[k].slice(0, 400)}\n  C:  ${c[k].slice(0, 400)}\n  JS: ${js[k].slice(0, 400)}`);
  }
}
console.log(`cases ${N} (seed ${SEED}): ${Object.entries(tally).map(([k, v]) => `${k} ${v}`).join(', ')}`);
console.log(bad ? `SHIKI DIFF FAIL (${bad})` : 'SHIKI DIFF PASS');
console.log(`cases kept in ${dir}`);
process.exit(bad ? 1 : 0);
