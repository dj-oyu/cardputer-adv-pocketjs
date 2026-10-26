// vmrun-flags: --harden
// docs/vm/harden.md: the four rules, what stays allowed, and the lazy paths
// (a builtin first touched after the lock is born locked).
function t(name, f) {
  try { print(name, 'ok', String(f())); }
  catch (e) { print(name, e.name, e.message); }
}

// 1. no code from strings
t('eval', () => eval('1+1'));
t('eval-indirect', () => (0, eval)('1+1'));
t('eval-nonstring', () => eval(42));
t('Function', () => Function('return 1')());
t('new-Function', () => new Function('a', 'return a')(1));
t('ctor-of-arrow', () => (() => 0).constructor('return 1')());
t('async-ctor', () => (async function () {}).constructor('return 1'));
t('gen-ctor', () => (function* () {}).constructor('yield 1'));

// 2. no [[Prototype]] change after creation
const base = {k: 1};
t('setPrototypeOf', () => Object.setPrototypeOf({}, base));
t('setPrototypeOf-same', () => { const o = {}; return Object.setPrototypeOf(o, Object.prototype) === o; });
t('Reflect.setPrototypeOf', () => Reflect.setPrototypeOf({}, base));
t('__proto__=', () => { const o = {}; o.__proto__ = base; return o.k; });
t('literal-__proto__', () => ({__proto__: base}).k);
t('Object.create', () => Object.create(base).k);
t('class-extends', () => { class A { f() { return 2; } } class B extends A {} return new B().f(); });

// 3. intrinsics locked; failed writes throw; shadowing allowed
t('Array.prototype.x=', () => { Array.prototype.x = 1; return [].x; });
t('Array.prototype.map=', () => { Array.prototype.map = null; return typeof [].map; });
t('delete-map', () => delete Array.prototype.map);
t('Math.max=', () => { Math.max = () => 0; return Math.max(1, 2); });
t('Math.newfn=', () => { Math.foo = 1; return Math.foo; });
t('Math.max.foo=', () => { Math.max.foo = 1; return Math.max.foo; });
t('Object.prototype.toString=', () => { Object.prototype.toString = () => 'x'; return String({}); });
t('defineProperty-proto', () => Object.defineProperty(Array.prototype, 'y', {value: 1}));
t('global-Math=', () => { Math = {}; return typeof Math.max; });
t('global-Array=', () => { globalThis.Array = 0; return typeof Array; });
t('delete-global-JSON', () => delete globalThis.JSON);
t('isFrozen-Array.prototype', () => Object.isFrozen(Array.prototype));
t('isFrozen-Math', () => Object.isFrozen(Math));
t('shadow-toString', () => { const o = {}; o.toString = () => 'mine'; return String(o); });
t('shadow-name', () => { const e = Object.create(Error.prototype); e.name = 'PocketError'; return e.name; });
t('own-write', () => { const o = {a: 1}; o.a = 2; o.b = 3; return o.a + o.b; });
t('user-freeze-throws', () => { const o = Object.freeze({a: 1}); o.a = 2; return o.a; });
t('user-proto-mutable', () => { function F() {} F.prototype.m = () => 5; return new F().m(); });
t('globals-own', () => { globalThis.mine = 1; mine = 2; return mine; });
// lazy: first touched after the lock
t('lazy-entry', () => { const d = Object.getOwnPropertyDescriptor(String.prototype, 'padEnd'); return d.writable + ',' + d.configurable; });
t('lazy-fn-locked', () => Object.isExtensible(String.prototype.padStart));
t('lazy-class', () => { const m = new Map([[1, 2]]); return Object.isFrozen(Map.prototype) + ',' + m.get(1); });
t('lazy-class-write', () => { Map.prototype.get = null; return 1; });
t('lazy-typed', () => { Uint8Array.prototype.fill = null; return 1; });
t('lazy-typed-ok', () => new Uint8Array([1, 2, 3]).map(x => x * 2).join());
t('map-iter-proto', () => { const it = new Map().entries(); Object.getPrototypeOf(it).next = null; return 1; });

// 4. no species
t('species-map', () => { const a = [1, 2]; a.constructor = function () { throw 0; }; return a.map(x => x + 1).join(); });
t('species-sym', () => { class A extends Array {} return new A(1, 2).map(x => x) instanceof A; });
t('promise-then', () => { const p = Promise.resolve(1); p.constructor = function () { throw 0; }; return p.then(x => x) instanceof Promise; });

// ordinary use still works
t('json', () => JSON.stringify({a: [1, {b: 2}]}));
t('math', () => Math.max(1, 5, 3) + Math.floor(2.5));
t('array', () => [3, 1, 2].sort().concat([4]).slice(1).filter(x => x > 1).join());
t('apply', () => Math.max.apply(null, [1, 9, 3]));
t('string', () => 'abc'.padEnd(5, '.').toUpperCase());
t('iter', () => [...'héllo'].length);
t('regexp', () => 'a1b22'.replace(/\d+/g, '#').split('#').join('|'));
t('promise-finally', () => { let s = ''; Promise.resolve(1).finally(() => { s += 'f'; }).then(v => print('finally-then', v, s)); return 'queued'; });
