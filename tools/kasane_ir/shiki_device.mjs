// The host half of Shiki's on-device differential test (apps/shikitest).
//
//   node tools/kasane_ir/shiki_device.mjs LOG.txt [--seed S] [--cases N]
//   node tools/kasane_ir/shiki_device.mjs --expect [--seed S] [--cases N]
//
// Loads apps/shikitest/shiki_test.js as it is (without pocket, so only its
// generator and hash are defined), makes the same cases the device made,
// runs them through the C engine (tools/kasane_contract/shiki_run.c, ASan)
// and the host reference (tools/kasane_ir/shiki_ref.mjs), which must agree,
// and compares every "SK <i> <case hash> <result hash> <status>" line of the
// device's log with them:
//   case hash differs   the device's JS made a different case (QuickJS and
//                       Node disagree on the generator's arithmetic or on
//                       String(number)); the result is then not compared.
//   result differs      the engine on the device, or the sec.2.4 conversion
//                       of a JS number in its soft-float double, disagrees.
// --expect prints the lines the device should log.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import {execFileSync} from 'node:child_process';

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../..');
const argv = process.argv.slice(2);
const opt = (name, d) => { const i = argv.indexOf(name); return i < 0 ? d : argv[i + 1]; };
const app = fs.readFileSync(path.join(ROOT, 'apps/shikitest/shiki_test.js'), 'utf8');
// The device's own numbers, read from the app so they cannot drift.
const SEED = Number(opt('--seed', /SEED = (\d+)/.exec(app)[1])), N = Number(opt('--cases', /N = (\d+)/.exec(app)[1]));
const ctx = {};
vm.runInNewContext(app, ctx);
const next = ctx.shikiGen(SEED), hash = ctx.shikiHash;

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'shiki-device-'));
const cases = [];
for (let i = 0; i < N; i++) cases.push(next().line);
fs.writeFileSync(path.join(dir, 'cases.txt'), cases.join('\n') + '\n');
const runner = path.join(dir, 'shiki_run');
execFileSync('gcc', ['-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover', '-Imain/ui/kasane',
  'main/ui/kasane/ksn_shiki.c', 'tools/kasane_contract/shiki_run.c', '-lm', '-o', runner], {cwd: ROOT, stdio: 'inherit'});
const big = {maxBuffer: 1 << 30, cwd: ROOT};
const c = execFileSync(runner, [path.join(dir, 'cases.txt')], big).toString().trimEnd().split('\n');
const js = execFileSync('node', [path.join(ROOT, 'tools/kasane_ir/shiki_ref.mjs'), path.join(dir, 'cases.txt')], big).toString().trimEnd().split('\n');
let hostBad = 0;
for (let i = 0; i < N; i++) if (c[i] !== js[i]) hostBad++;
if (hostBad) { console.log(`C and reference disagree on ${hostBad} of these cases: run shiki_diff.mjs first`); process.exit(1); }
const want = c.map((line, i) => `SK ${i} ${hash(cases[i])} ${hash(line)} ${line === 'REJECT' ? 'REJECT' : line.split(' ')[1]}`);

if (argv.includes('--expect')) { console.log(want.join('\n')); process.exit(0); }
const logFile = argv.find(a => !a.startsWith('--') && a !== opt('--seed') && a !== opt('--cases'));
if (!logFile) throw new Error('usage: shiki_device.mjs LOG.txt | --expect');
const got = new Map();
for (const raw of fs.readFileSync(logFile, 'utf8').split('\n')) {
  const m = /SK (\d+) ([0-9a-f]{8}) ([0-9a-f]{8}) ([A-Z]+)/.exec(raw);
  if (m) got.set(Number(m[1]), m[0]);
}
let caseBad = 0, resultBad = 0, missing = 0;
for (let i = 0; i < N; i++) {
  const g = got.get(i);
  if (!g) { missing++; continue; }
  if (g === want[i]) continue;
  const [, , gc] = g.split(' '), [, , wc] = want[i].split(' ');
  if (gc !== wc) caseBad++; else resultBad++;
  if (caseBad + resultBad <= 5) console.log(`DIFF case ${i}\n  device: ${g}\n  host:   ${want[i]}\n  ${cases[i].slice(0, 300)}\n  host line: ${c[i].slice(0, 300)}`);
}
const tally = {};
for (const w of want) { const s = w.split(' ')[4]; tally[s] = (tally[s] ?? 0) + 1; }
console.log(`cases ${N} (seed ${SEED}): ${Object.entries(tally).map(([k, v]) => `${k} ${v}`).join(', ')}`);
console.log(`device lines ${got.size}, missing ${missing}, case differs ${caseBad}, result differs ${resultBad}`);
const ok = !missing && !caseBad && !resultBad;
console.log(ok ? 'SHIKI DEVICE PASS' : 'SHIKI DEVICE FAIL');
process.exit(ok ? 0 : 1);
