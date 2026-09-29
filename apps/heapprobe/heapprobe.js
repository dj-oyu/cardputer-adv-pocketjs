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
