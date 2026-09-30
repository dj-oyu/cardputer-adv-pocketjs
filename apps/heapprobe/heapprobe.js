// Heap probe variants (POCKET_HEAPPROBE only; docs/vm/spread-eval-oom.md).
// NOT one program: each "//@ name" line starts a separate source, evaluated
// alone in a fresh guest ('Q<index>[,<limit>]\n' over USB, from 0).
// Every variant defines frame() so it runs as an app; X is the construct,
// at top level (eval), in a function called at top level (eval), or in
// the first frame() (a turn after the evaluation).
// "@derby" / "@derby-spread": apps/derby/derby_watch.js as shipped, or with
// GK built by the spread again (the source that ran out of heap).
// "@derby-flat" / "@derby-split3|4": the same app unwrapped into one global
// script, or cut into 3 or 4 loaded by __hpLoad(k) (tools/heapprobe_split.py).
// alias-*: the other int / int32_t pointer sites of quickjs.c, as checks.
// "@derby-load3|4": the same 3 or 4 scripts through pocket.app.load('s3c1')..
// appload-*: pocket.app.load() itself (docs/vm/eval-peak.md sec.7), on the
// chunks of apps/heapprobe/appload_chunks.txt and `big` (derby's first third).
// appload-oom wants a limit ('Q44,80000'): `big` must not fit beside the
// ballast, and must once the ballast is gone.
// "@imp-single|load3|import3": one generated 32 KB program as one script,
// three scripts through pocket.app.load(), three modules through static import
// (tools/heapprobe_import_gen.py; docs/vm/eval-peak.md sec.9). "@module" on a
// section's first line evaluates it as a module entry; import-*: its checks,
// on apps/heapprobe/import_chunks.txt.
//@ baseline
var n = 1; console.log('HP_EVAL ok'); globalThis.frame = () => {};
//@ spread-mixed-top
const GK = [...'adesr1,/', 'tab']; console.log('HP_EVAL ok ' + GK.length); globalThis.frame = () => {};
//@ spread-str8-top
const a = [...'adesr1,/']; console.log('HP_EVAL ok ' + a.length); globalThis.frame = () => {};
//@ spread-abc-top
const a = [...'abc']; console.log('HP_EVAL ok ' + a.length); globalThis.frame = () => {};
//@ spread-str1-top
const s = 'a', a = [...s]; console.log('HP_EVAL ok ' + a.length); globalThis.frame = () => {};
//@ spread-str30-top
const s = 'abcdefghijklmnopqrstuvwxyz0123', a = [...s]; console.log('HP_EVAL ok ' + a.length); globalThis.frame = () => {};
//@ spread-str100-top
const s = 'x'.repeat(100), a = [...s]; console.log('HP_EVAL ok ' + a.length); globalThis.frame = () => {};
//@ spread-empty-top
const a = [...'']; console.log('HP_EVAL ok ' + a.length); globalThis.frame = () => {};
//@ array-from-str-top
const a = Array.from('adesr1,/'); console.log('HP_EVAL ok ' + a.length); globalThis.frame = () => {};
//@ split-top
const a = 'adesr1,/'.split(''); console.log('HP_EVAL ok ' + a.length); globalThis.frame = () => {};
//@ spread-arr-top
const b = [1, 2, 3], a = [...b, 'x']; console.log('HP_EVAL ok ' + a.length); globalThis.frame = () => {};
//@ spread-set-top
const a = [...new Set([1, 2, 3])]; console.log('HP_EVAL ok ' + a.length); globalThis.frame = () => {};
//@ spread-arr-iter-top
const a = [...[1, 2, 3].values()]; console.log('HP_EVAL ok ' + a.length); globalThis.frame = () => {};
//@ spread-gen-top
function* g() { yield 1; yield 2; } const a = [...g()]; console.log('HP_EVAL ok ' + a.length); globalThis.frame = () => {};
//@ for-of-str-top
let k = 0; for (const c of 'adesr1,/') k++; console.log('HP_EVAL ok ' + k); globalThis.frame = () => {};
//@ for-of-arr-top
let k = 0; for (const c of [1, 2, 3]) k++; console.log('HP_EVAL ok ' + k); globalThis.frame = () => {};
//@ str-iter-next-top
const it = 'ab'[Symbol.iterator](); const r1 = it.next(), r2 = it.next(), r3 = it.next(); console.log('HP_EVAL ok ' + r1.value + r2.value + r3.done); globalThis.frame = () => {};
//@ destructure-str-top
const [p, q] = 'ab'; console.log('HP_EVAL ok ' + p + q); globalThis.frame = () => {};
//@ keys-entries-top
const o = {a: 1, b: 2}, a = Object.keys(o), e = Object.entries(o); console.log('HP_EVAL ok ' + a.length + e.length); globalThis.frame = () => {};
//@ template-top
const x = 3, s = `a${x}b${x + 1}`; console.log('HP_EVAL ok ' + s); globalThis.frame = () => {};
//@ spread-call-top
const m = Math.max(...[1, 5, 3]); console.log('HP_EVAL ok ' + m); globalThis.frame = () => {};
//@ spread-call-str-top
const m = String.fromCharCode(...'ab'.split('').map(c => c.charCodeAt(0))); const n2 = Math.max(...'123'); console.log('HP_EVAL ok ' + m + n2); globalThis.frame = () => {};
//@ spread-mixed-iife
(function () { const GK = [...'adesr1,/', 'tab']; console.log('HP_EVAL ok ' + GK.length); })(); globalThis.frame = () => {};
//@ spread-mixed-arrow-iife
(() => { const GK = [...'adesr1,/', 'tab']; console.log('HP_EVAL ok ' + GK.length); })(); globalThis.frame = () => {};
//@ for-of-str-iife
(function () { let k = 0; for (const c of 'adesr1,/') k++; console.log('HP_EVAL ok ' + k); })(); globalThis.frame = () => {};
//@ spread-mixed-frame
let d = 0; console.log('HP_EVAL ok'); globalThis.frame = () => { if (d++) return; const GK = [...'adesr1,/', 'tab']; console.log('HP_FRAME ok ' + GK.length); };
//@ spread-str8-frame
let d = 0; console.log('HP_EVAL ok'); globalThis.frame = () => { if (d++) return; const a = [...'adesr1,/']; console.log('HP_FRAME ok ' + a.length); };
//@ for-of-str-frame
let d = 0; console.log('HP_EVAL ok'); globalThis.frame = () => { if (d++) return; let k = 0; for (const c of 'adesr1,/') k++; console.log('HP_FRAME ok ' + k); };
//@ spread-arr-frame
let d = 0; console.log('HP_EVAL ok'); globalThis.frame = () => { if (d++) return; const b = [1, 2, 3], a = [...b, 'x']; console.log('HP_FRAME ok ' + a.length); };
//@ derby
@derby
//@ derby-spread
@derby-spread
//@ alias-parseint
const v = [parseInt('ff', 16), parseInt('11', 2), (255).toString(16)]; console.log('HP_EVAL ok ' + v); globalThis.frame = () => {};
//@ alias-promise-all
Promise.all([1, Promise.resolve(2), 3]).then(a => console.log('HP_FRAME ok ' + a)); console.log('HP_EVAL ok'); globalThis.frame = () => {};
//@ alias-stack-line
globalThis.frame = () => {};

try { null.x; } catch (e) { console.log('HP_EVAL ok ' + String(e.stack).replace(/\s+/g, ' ')); }
//@ alias-normalize
const s = 'é'.normalize('NFD'), t = 'é'.normalize('NFC'); console.log('HP_EVAL ok ' + s.length + ' ' + t.length + ' ' + t.charCodeAt(0)); globalThis.frame = () => {};
//@ derby-flat
@derby-flat
//@ derby-split3
@derby-split3
//@ derby-split4
@derby-split4
//@ derby-bc
@derby-bc
//@ lazy-frame-src
@chunks3
let n = 0; console.log('HP_EVAL ok'); globalThis.frame = () => { if (++n === 20) { const t = Date.now(); try { __hpLoad(0); console.log('HP_FRAME ok ms=' + (Date.now() - t)); } catch (e) { console.log('HP_FRAME fail ' + e); } } else if (n === 40) console.log('HP_ALIVE ' + n); };
//@ lazy-frame-bc
let n = 0; console.log('HP_EVAL ok'); globalThis.frame = () => { if (++n === 20) { const t = Date.now(); try { __hpLoadBC(); console.log('HP_FRAME ok ms=' + (Date.now() - t)); } catch (e) { console.log('HP_FRAME fail ' + e); } } else if (n === 40) console.log('HP_ALIVE ' + n); };
//@ derby-load3
@derby-load3
//@ derby-load4
@derby-load4
//@ appload-api
(() => { const L = pocket.app.load, R = []; function t(n, f) { try { R.push(n + '=' + f()); } catch (e) { R.push(n + '=' + e.code + '/' + e.outcome + '/' + e.retryable); } }
t('cap', () => JSON.stringify(pocket.capabilities.get('app.load').limits)); t('ok', () => L('ok')); t('ok2', () => L('ok')); t('use', () => alF() + alV + alG + alC.n + alS);
t('nf', () => L('nope')); t('arg', () => L(3)); t('bad', () => L('bad')); t('thr', () => L('thr')); t('thr2', () => L('thr')); t('self', () => L('self') + alSelf);
console.log('HP_EVAL ' + R.slice(0, 6).join(' ')); console.log('HP_EVAL2 ' + R.slice(6).join(' ')); let n = 0;
globalThis.frame = () => { if (++n === 10) { const t0 = Date.now(), a = L('mid'), t1 = Date.now(); let b; try { b = L('big'); } catch (e) { b = e.code + ' ' + e.message; }
console.log('HP_FRAME ok mid=' + a + ' ' + (t1 - t0) + 'ms sum=' + alMid.sum() + ' big=' + b + ' ' + (Date.now() - t1) + 'ms'); } else if (n === 40) console.log('HP_ALIVE ' + n); }; })();
//@ appload-oom
(() => { const L = pocket.app.load; let n = 0, ballast = new Uint8Array(20000), a = 'none', b = 'none'; console.log('HP_EVAL ok');
globalThis.frame = () => { ++n; if (n === 12) console.log('HP_FRAME ok first=' + a + ' retry=' + b); if (n === 40) console.log('HP_ALIVE ' + n); if (n !== 10) return;
try { L('big'); a = 'loaded'; } catch (e) { a = e === null ? 'null' : e.code + '/' + e.retryable + '/' + e.outcome; }
ballast = null; try { L('big'); b = 'loaded'; } catch (e) { b = e === null ? 'null' : e.code; } }; })();
//@ appload-uncaught
let n = 0; console.log('HP_EVAL ok'); globalThis.frame = () => { if (++n === 5) pocket.app.load('bad'); };
//@ appload-evalerr
pocket.app.load('thr'); globalThis.frame = () => {};
//@ imp-single
@imp-single
//@ imp-load3
@imp-load3
//@ imp-import3
@imp-import3
//@ import-api
@module
import { count, bump, name } from 'imok'; import { base } from 'imbase'; import * as cyc from 'imcycb';
bump(); let lm; try { pocket.app.load('imok'); lm = 'loaded'; } catch (e) { lm = e.code; }
console.log('HP_EVAL ok count=' + count + ' base=' + base + ' runs=' + globalThis.imBaseRuns + ' cyc=' + cyc.cab() + ' ' + name + ' loadModule=' + lm);
let n = 0, dyn = 'pending'; import('imbase').then(() => { dyn = 'resolved'; }, e => { dyn = String(e); });
globalThis.frame = () => { if (++n === 10) console.log('HP_FRAME ok count=' + count + ' dyn=' + dyn); else if (n === 40) console.log('HP_ALIVE ' + n); };
//@ import-syntax
@module
import { a } from 'imbad'; globalThis.frame = () => {};
//@ import-throw
@module
import { before } from 'imthrow'; globalThis.frame = () => {};
//@ import-tdz
@module
import { ta } from 'imtdza'; globalThis.frame = () => {};
//@ import-unknown
@module
import { x } from 'nope'; globalThis.frame = () => {};
//@ import-script
@module
import { x } from 'ok'; globalThis.frame = () => {};
//@ import-tla
@module
import { x } from 'imtla'; globalThis.frame = () => {};
