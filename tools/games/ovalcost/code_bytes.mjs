// What a variant's code keeps in DERBY WATCH's guest heap after evaluation,
// measured with the device-sized QuickJS (the harness built by run_derby.py
// --m32, DERBY_EVAL_ONLY): the app of a git ref, lowered, with each variant of
// variants.mjs applied. Tables a variant fills at run time (enter('pad')) are
// not in it: the m32 game run's live peak shows those.
//
//   node tools/games/ovalcost/code_bytes.mjs [--ref 45f309f] [variant ...]   (WSL)
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { patchApp } from './variants.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const a = process.argv.slice(2), ri = a.indexOf('--ref'), REF = ri < 0 ? '45f309f' : a[ri + 1];
const V = a.filter((x, i) => x !== '--ref' && x !== '--export' && x !== '--oval' && (ri < 0 || i !== ri + 1));
const BIN = path.join(ROOT, '.cache/derby_host/m32/test-derby'), W = path.join(ROOT, '.cache/trigcull/code');
// --export: write the ref's apps/derby to .cache/trigcull/code_src/<ref> and
// stop (Windows: WSL's git may not read a worktree's gitdir, CLAUDE.md); the
// measuring run (WSL) reads that copy.
const src = path.join(ROOT, '.cache/trigcull/code_src', REF, 'apps/derby');
if (process.argv.includes('--export')) {
  fs.rmSync(src, { recursive: true, force: true }); fs.mkdirSync(src, { recursive: true });
  for (const f of execFileSync('git', ['ls-tree', '--name-only', `${REF}:apps/derby/`], { cwd: ROOT, encoding: 'utf8' }).split('\n').filter(Boolean))
    fs.writeFileSync(path.join(src, f), execFileSync('git', ['show', `${REF}:apps/derby/${f}`], { cwd: ROOT }));
  console.log('exported', REF); process.exit(0);
}
fs.rmSync(W, { recursive: true, force: true });
const low = path.join(W, 'low/apps/derby');
execFileSync('node', [path.join(ROOT, 'tools/kasane_ir/lower_plans.mjs'), src, low], { cwd: ROOT, stdio: 'ignore' });
function after(v) {
  const dir = path.join(W, v || 'app', 'apps/derby');
  fs.mkdirSync(dir, { recursive: true });
  for (const f of fs.readdirSync(low)) fs.copyFileSync(path.join(low, f), path.join(dir, f));
  const rd = f => fs.readFileSync(path.join(dir, f), 'utf8');
  const files = patchApp({ view: rd('derby_view.js'), pan: rd('derby_pan.js'), scene: rd('derby_scene.js') }, v);
  fs.writeFileSync(path.join(dir, 'derby_view.js'), files.view);
  fs.writeFileSync(path.join(dir, 'derby_pan.js'), files.pan);
  fs.writeFileSync(path.join(dir, 'derby_scene.js'), files.scene);
  // --oval: every race on the oval (the first paddock, entered while the app
  // evaluates, fills a variant's per-race table at its oval size).
  if (process.argv.includes('--oval')) {
    const w = rd('derby_watch.js');
    if (!/const OV = \[[\d.]+,/.test(w)) throw new Error('derby_watch.js: OV changed');
    fs.writeFileSync(path.join(dir, 'derby_watch.js'), w.replace(/const OV = \[[\d.]+,/, 'const OV = [1,'));
  }
  const p = spawnSync(BIN, [], { cwd: ROOT, encoding: 'utf8', env: { ...process.env, DERBY_APP_DIR: dir, DERBY_EVAL_ONLY: '1' } });
  const m = /peak during eval (\d+) \(\+\d+\), after eval (\d+)/.exec(p.stdout);
  if (!m) throw new Error(`${v}: ${p.stdout.slice(-600)}`);
  return [+m[1], +m[2]];
}
const b = after(null);
console.log(`ref ${REF}: peak during eval ${b[0]}, after eval ${b[1]}`);
for (const v of V) { const c = after(v); console.log(`${v}: peak during eval ${c[0] - b[0] >= 0 ? '+' : ''}${c[0] - b[0]}, after eval ${c[1] - b[1] >= 0 ? '+' : ''}${c[1] - b[1]}`); }
