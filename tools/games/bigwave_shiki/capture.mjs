// BIG WAVE's move to Shiki (v2.0 step 3), the capture half: runs the float
// VM version of apps/bigwave/big_wave.js (git show 885fb18:apps/bigwave/big_wave.js
// > old.js) under Node with a fake pocket and records every draw: the old
// program's rows, its key, set and tier, and the inputs. Scripted play: title
// (auto), the load tiers, start, six rounds of a key schedule.
//
//   node tools/games/bigwave_shiki/capture.mjs OLD_BIG_WAVE.js draws.json
//   node tools/games/bigwave_shiki/compare.mjs draws.json apps/bigwave/big_wave_prog.js
import fs from 'node:fs';
let src = fs.readFileSync(process.argv[2], 'utf8');
src = src.replace("const SETK = ['cu', 'sh', 'se', 'sk', 'fo'];", "const SETK = ['cu', 'sh', 'se', 'sk', 'fo']; globalThis.__bw = {args, sprite, SETS, K, TIER, C, PF};");
src = src.replace('function spec(k, set, tier) {', 'function spec(k, set, tier) { globalThis.__spec = [k, set, tier];');
const handles = new Map(); let next = 1; const draws = [];
let pressed = new Set(), heldk = new Set();
const P = {
  register(rows, pts) { const h = next++; handles.set(h, {rows, pts, spec: globalThis.__spec}); return h; },
  unregister(h) { handles.delete(h); },
  draw(h, inp) { const r = handles.get(h); draws.push({rows: r.rows, spec: r.spec, inputs: inp.slice()}); },
  beginFrame() {}, commit() {}, resource() { return {}; }
};
const tx = new Proxy({}, {get: () => () => ({setRect() {}, setText() {}})});
globalThis.pocket = {
  kasane: {procedural: P, replace(f) { f(tx); }, patch(f) { f(tx); }},
  input: {keys: {pressed: k => pressed.has(k), held: k => heldk.has(k)}},
  capabilities: {get: () => ({supported: false})}, audio: null
};
globalThis.console = console;
new Function(src)();
const step = (n, keys = [], tap = []) => { for (let i = 0; i < n; i++) { pressed = new Set(i ? [] : tap); heldk = new Set(keys); globalThis.frame(0); } };
step(200);                        // title, auto ride, loads everything
for (const t of ['3', '4', '2']) { step(30, [], [t]); step(60); }   // tiers
step(5, [], ['1']);               // start
const plan = [[60, ['e']], [40, ['a']], [40, ['d']], [30, ['.']], [20, [], [';']], [60, ['e', 'd']], [40, [',']], [60, ['e']]];
for (let r = 0; r < 6; r++) for (const [n, k, t] of plan) step(n, k, t || []);
fs.writeFileSync(process.argv[3], JSON.stringify({draws, bw: {}}));
console.log('draws', draws.length, 'by key', JSON.stringify(draws.reduce((m, d) => (m[d.spec[0]] = (m[d.spec[0]] || 0) + 1, m), {})));
