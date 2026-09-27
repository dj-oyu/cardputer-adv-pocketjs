// vmrun-flags: --profile device
// Regression for docs/vm/turn-cpi.md sec.4 (R4a): the first read of a lazy
// list entry (POCKET_VM_LAZY_BUILTINS) marked it done before putting it in
// the shape, and an out-of-memory in between left the name in neither place:
// it read as undefined for the rest of the run. On the device pocket.fs.open
// vanished that way; on the host Object.fromEntries did, at --fail-alloc 707.
//
// Each name below is untouched until its own turn. For each, fill the heap to
// the limit with small objects, release r of them, and read the name --
// r = 0, 1, 2, ... until the read succeeds, so the first materialization is
// tried at every margin on the way, including the one where the shape can
// not grow. Afterwards every name must still be a function.
const names = [
  [Object, 'fromEntries'], [Object, 'getOwnPropertyDescriptors'], [Object, 'isFrozen'],
  [Math, 'hypot'], [Math, 'cbrt'], [Math, 'log1p'], [Math, 'expm1'], [Math, 'fround'],
  [Math, 'sinh'], [Math, 'cosh'], [Math, 'tanh'], [Math, 'asinh'],
  [Reflect, 'ownKeys'], [Reflect, 'apply'], [Reflect, 'isExtensible'],
  [Number, 'isSafeInteger'], [String, 'raw'], [Array, 'of'],
];
let refused = 0;
for (const [o, k] of names) {
  for (let r = 0; r < 200; r++) {
    let hog = [];
    try { for (;;) hog.push({ n: hog.length }); } catch (e) {}
    hog.length = hog.length > r ? hog.length - r : 0;
    let ok = false;
    try { void o[k]; ok = true; } catch (e) { refused++; }
    hog = null;
    if (ok) break;
  }
}
print(names.map(([o, k]) => typeof o[k]).join(','));
print('#info refused ' + refused);
