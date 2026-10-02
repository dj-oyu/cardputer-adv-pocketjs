// vmrun-flags: --test262
// Both macro states must pass this same result. Coercion precedes bounds.
(function () {
  let checks = 0;
  function eq(a, b, label) { checks++; if (!Object.is(a, b)) throw Error(label + ': ' + String(a) + ' != ' + String(b)); }
  function throws(f, ctor, label) { let caught; try { f(); } catch (e) { caught = e; } checks++; if (!(caught instanceof ctor)) throw Error(label); }
  const types = [Int8Array, Uint8Array, Int16Array, Uint16Array, Int32Array, Uint32Array];
  const inputs = [-2147483648, -65537, -32769, -257, -129, -1, 0, 1, 127, 128, 255, 256, 32767, 32768, 65535, 65536, 2147483647, 2147483648, 4294967295, 4294967296, -0, 1.75, -1.75, NaN, Infinity, -Infinity, true, false, null, undefined, '257'];
  for (let t = 0; t < types.length; t++) {
    const C = types[t], a = new C(2), bits = [8, 8, 16, 16, 32, 32][t], signed = !(t & 1);
    for (const value of inputs) {
      let expected = Number(value) | 0;
      if (bits < 32) { expected &= (1 << bits) - 1; if (signed && expected >= (1 << (bits - 1))) expected -= 1 << bits; }
      else if (!signed) expected >>>= 0;
      a[0] = value; eq(a[0], expected, C.name + ' immediate/conversion');
      a['1'] = value; eq(a[1], expected, C.name + ' string index');
      a[-0] = value; eq(a[0], expected, C.name + ' negative zero');
    }
    for (const index of [0, 2, -1, '-0', 'Infinity', 0.5]) {
      let called = 0;
      a[index] = { valueOf() { called++; return 257; } };
      eq(called, 1, 'coerce before bounds ' + index);
      throws(() => { a[index] = Symbol(); }, TypeError, 'symbol before bounds');
      throws(() => { a[index] = 1n; }, TypeError, 'bigint before bounds');
      const sentinel = {};
      let caught;
      try { a[index] = { valueOf() { throw sentinel; } }; } catch (e) { caught = e; }
      eq(caught, sentinel, 'conversion exception identity');
    }
    let numericSetter = 0, ordinarySetter = 0;
    Object.defineProperty(C.prototype, '0', { set(v) { numericSetter++; }, configurable: true });
    Object.defineProperty(C.prototype, 'foo', { set(v) { ordinarySetter += v; }, configurable: true });
    try { a[0] = 7; a[9] = 7; a.foo = 3; eq(numericSetter, 0, 'integer indexed ignores prototype setter'); eq(ordinarySetter, 3, 'ordinary setter'); }
    finally { delete C.prototype[0]; delete C.prototype.foo; }
    let side = 0; const obj = { valueOf() { side++; return 5; } };
    a['01'] = obj; eq(a['01'], obj, 'noncanonical index property'); eq(side, 0, 'noncanonical no coercion');
    const b = new C(2); $262.detachArrayBuffer(b.buffer);
    b[0] = 7; eq(b[0], undefined, 'detached integer ignored');
    b[0] = { valueOf() { side++; return 5; } }; eq(side, 1, 'detached still coerces');
    throws(() => { b[0] = Symbol(); }, TypeError, 'detached symbol');
    const c = new C(2);
    c[0] = { valueOf() { $262.detachArrayBuffer(c.buffer); return 9; } };
    eq(c[0], undefined, 'detach during conversion');
    const rab = new ArrayBuffer(C.BYTES_PER_ELEMENT * 4, { maxByteLength: C.BYTES_PER_ELEMENT * 8 });
    const d = new C(rab, C.BYTES_PER_ELEMENT, 2);
    d[0] = { valueOf() { rab.resize(0); return 9; } };
    eq(d[0], undefined, 'resize out of bounds during conversion');
    d[0] = { valueOf() { rab.resize(C.BYTES_PER_ELEMENT * 4); return 9; } };
    eq(d[0], 9, 'resize in bounds during conversion');
    const immutable = new C(new ArrayBuffer(C.BYTES_PER_ELEMENT * 2).transferToImmutable());
    immutable[0] = 7; eq(immutable[0], 0, 'immutable immediate unchanged');
    let immutableCoercion = 0;
    immutable[0] = { valueOf() { immutableCoercion++; return 7; } };
    eq(immutable[0], 0, 'immutable conversion unchanged');
    eq(immutableCoercion, 1, 'immutable coerces before rejection');
    (function () { 'use strict'; b[0] = 17; a[99] = 17; })();
    eq(a[99], undefined, 'strict OOB ignored');
  }
  let setters = 0;
  const ordinary = { set 0(v) { setters += v; } }; ordinary[0] = 4;
  const proxy = new Proxy({}, { set(target, key, value) { eq(key, '0', 'proxy key'); setters += value; return true; } }); proxy[0] = 5;
  eq(setters, 9, 'opcode fallback setters');
  const clamped = new Uint8ClampedArray(3); clamped[0] = -1; clamped[1] = 300; clamped[2] = 2.5;
  eq(Array.from(clamped).join(','), '0,255,2', 'clamped unchanged');
  const big = new BigInt64Array(1); throws(() => { big[0] = 3; }, TypeError, 'bigint unchanged');
  const floats = new Float64Array(1); floats[0] = -0; eq(floats[0], -0, 'float unchanged');
  const invalid = new Int8Array(1); let invalidCoercions = 0;
  invalid[NaN] = { valueOf() { invalidCoercions++; return 1; } };
  invalid['NaN'] = { valueOf() { invalidCoercions++; return 1; } };
  print('typed-put NaN-index baseline coercions', invalidCoercions);
  print('typed-put checks', checks);
})();
