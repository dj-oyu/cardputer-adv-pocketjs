// vmrun-flags: --harden --test262
// docs/vm/rom-intrinsics.md R1: what a RAM object may do with an intrinsic
// that lives in ROM. Run as is (harden) and under --immortal (the intrinsics
// immortal, as the snapshot will hold them); the output must not change.
// Each case is a store a naive ROM would take: a refcount, a GC mark, the
// is_prototype bit, the extensible bit, the weak list.
function t(name, f) {
  try { print(name, 'ok', String(f())); }
  catch (e) { print(name, e.name, e.message); }
}

// RAM objects whose [[Prototype]] is an intrinsic (set_proto, is_prototype)
t('create-Math', () => Object.create(Math).max(1, 5));
t('create-ArrayProto', () => { const o = Object.create(Array.prototype); o.push(7); return o.length; });
t('extends-Array', () => { class A extends Array {} const a = new A(); a.push(1, 2); return a.length + ' ' + (a instanceof Array); });
t('extends-Error', () => { class E extends Error {} return new E('m').message + ' ' + (new E() instanceof Error); });
t('extends-Map', () => { class M extends Map {} const m = new M([[1, 2]]); return m.get(1); });
t('proto-identity', () => Object.getPrototypeOf([]) === Array.prototype && [].map === Array.prototype.map);

// freeze and friends on an intrinsic: already locked, no store
t('freeze-Math', () => Object.freeze(Math) === Math && Object.isFrozen(Math));
t('preventExtensions-ArrayProto', () => Object.preventExtensions(Array.prototype) === Array.prototype);
t('seal-JSON', () => Object.isSealed(Object.seal(JSON)));
t('freeze-fn', () => Object.isFrozen(Object.freeze(Math.max)));

// weak references to an intrinsic are refused (RD6); to a RAM object, fine
t('WeakRef-Math', () => new WeakRef(Math));
t('WeakMap-ArrayProto', () => new WeakMap().set(Array.prototype, 1));
t('WeakSet-fn', () => new WeakSet().add(Math.max));
t('FinReg-Math', () => new FinalizationRegistry(() => {}).register(Math, 1));
t('WeakRef-ram', () => typeof new WeakRef({}).deref());
t('WeakMap-ram', () => { const k = {}; return new WeakMap().set(k, 3).get(k); });

// refcount churn: many dups and frees of the same intrinsics
t('churn', () => {
  let a = [];
  for (let i = 0; i < 100000; i++) a.push(Math, Math.max, Array.prototype, JSON);
  const n = a.length;
  a = null;
  let s = 0;
  for (let i = 0; i < 100000; i++) s += Math.max(i & 7, 3);
  return n + ' ' + s;
});

// GC with RAM cycles that point into the intrinsics (gc_decref_child etc.)
t('gc-cycles', () => {
  const keep = [];
  for (let r = 0; r < 1000; r++) {
    const o = {m: Math, p: Array.prototype, f: Math.sin};
    o.self = o;
    const d = Object.create(o.p);
    d.back = o;
    if (r % 100 === 0) keep.push(d);
    $262.gc();
  }
  return keep.length + ' ' + keep[3].back.m.max(2, 9) + ' ' + (keep[9] instanceof Array);
});
t('after-gc', () => [1, 2, 3].map(x => Math.sqrt(x * x)).join() + ' ' + JSON.stringify({a: [1]}));

// realm-free C functions (RD2): native calls, getters, the Math fast path
t('cfunc', () => [Math.min(4, 2), Math.imul(3, 5), 'ab'.toUpperCase(), [3, 1, 2].sort().join(), Object.keys({x: 1})[0]].join());
t('getter', () => [new Map([[1, 1]]).size, Symbol.iterator.description, [].constructor === Array].join());
t('bound', () => Math.max.bind(null, 10)(3));
t('throw-native', () => { try { null.x; } catch (e) { return e instanceof TypeError; } });
