// vmrun-flags: --host-events
// F3b (docs/vm/builtin-floor-plan.md sec.17): typed-array classes made on
// first use must look exactly like eager ones. The expected file is made by
// the eager build. First, before any global is read: a Uint8Array made by
// native code (host.bytes = JS_NewUint8ArrayCopy, pocket.fs's path) has to
// get the same prototype and constructor the global will later give.
// The prototype's constructor is overwritten BEFORE the global is read: the
// binding must still be the real constructor, not whatever is there now.
const u = host.bytes(4);
const up = Object.getPrototypeOf(u), c0 = up.constructor;
up.constructor = 'x';
const out = [];
const p = (...a) => out.push(a.join(' '));
p('native-first', Array.from(u).join(','), c0 === Uint8Array, typeof Uint8Array,
  up === Uint8Array.prototype, u instanceof Uint8Array);
up.constructor = c0;

// The global object's keys, in order, and the bindings' flags, cold.
p('global-keys', Object.getOwnPropertyNames(globalThis).join(','));
const d = Object.getOwnPropertyDescriptor(globalThis, 'Int16Array');
p('global-desc', d.writable, d.enumerable, d.configurable, typeof d.value, d.value.name);
p('in', 'Float16Array' in globalThis, Object.prototype.hasOwnProperty.call(globalThis, 'DataView'));

// Deleted or overwritten before anything made the class.
p('delete', delete globalThis.Float32Array, typeof globalThis.Float32Array);
globalThis.Int8Array = 5;
p('overwrite', Int8Array, typeof Object.getOwnPropertyDescriptor(globalThis, 'Int8Array').value);

// %TypedArray% and its identities.
const TA = Object.getPrototypeOf(Int32Array);
p('base', TA === Object.getPrototypeOf(Uint16Array), TA.name, TA.prototype.toString === Array.prototype.toString,
  Object.getPrototypeOf(Float64Array.prototype) === TA.prototype, Uint8Array.prototype.constructor === Uint8Array);
p('statics', Uint8Array.BYTES_PER_ELEMENT, Float64Array.BYTES_PER_ELEMENT, BigUint64Array.name, Uint32Array.length,
  Object.getOwnPropertyNames(Uint8ClampedArray).join(','), Object.getOwnPropertyNames(Int16Array.prototype).join(','));

// Use: species, subclass, DataView, SharedArrayBuffer, Atomics.
const a = new Uint8Array([1, 2, 3]);
p('species', a.map(x => x * 2).join(','), a.subarray(1).constructor === Uint8Array, Object.prototype.toString.call(a));
class M extends Uint32Array {}
const m = new M(2);
p('subclass', m.length, m instanceof Uint32Array, Object.getPrototypeOf(M) === Uint32Array);
const dv = new DataView(new ArrayBuffer(4));
dv.setUint16(0, 0x1234);
p('dataview', dv.getUint8(0), dv.getUint8(1), Object.prototype.toString.call(dv));
const sab = new SharedArrayBuffer(8);
p('sab', sab.byteLength, Object.prototype.toString.call(sab), ArrayBuffer.isView(dv), ArrayBuffer.isView(sab));
const i32 = new Int32Array(sab);
Atomics.add(i32, 0, 7);
p('atomics', Atomics.load(i32, 0), i32.buffer === sab);

// Native again, after the global was read: still the same class.
const v = host.bytes(3);
p('native-after', Array.from(v).join(','), Object.getPrototypeOf(v) === Object.getPrototypeOf(u));
p('BigInt64Array', new BigInt64Array([5n])[0], typeof BigInt64Array.from);
p('global-keys-after', Object.getOwnPropertyNames(globalThis).join(','));
console.log(out.join('\n'));
