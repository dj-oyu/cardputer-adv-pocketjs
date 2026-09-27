// F1 (docs/vm/builtin-floor-plan.md): every path a builtin name takes when it
// lives in flash, printed so a -rom build must match the heap-atom build byte
// for byte. The expected file is made by the plain build.
const out = [];
const p = (...a) => out.push(a.join(' '));

// typeof hands out predefined names as string values (the escape path).
p('typeof', typeof 1, typeof 's', typeof {}, typeof undefined, typeof p, typeof Symbol());
p('typeof again', typeof 2 === 'number', (typeof 3).length);

// Enumeration of builtins, in definition order.
p('Math keys', Object.getOwnPropertyNames(Math).join(','));
p('Array.prototype', Object.getOwnPropertyNames(Array.prototype).join(','));
p('Uint8Array.prototype', Reflect.ownKeys(Object.getPrototypeOf(Uint8Array.prototype)).map(String).join(','));

// Function names and class names.
p('names', [].map.name, Math.max.name, Object.getOwnPropertyDescriptor(Map.prototype, 'size').get.name);
p('toString tags', Object.prototype.toString.call([]), Object.prototype.toString.call(new Map()));

// Symbols: a builtin name as a description, and uniqueness.
const s1 = Symbol(typeof 1), s2 = Symbol(typeof 1), s3 = Symbol('length');
p('symbols', s1.toString(), s1 === s2, s3.description, String(Symbol.iterator), Symbol.for('map') === Symbol.for('map'));

// "Infinity" is the one builtin name that is a canonical numeric string.
const ta = new Uint8Array(2);
ta.Infinity = 7; ta['-0'] = 7; ta.length2 = 1;
p('typed', ta.Infinity, ta['-0'], ta.length2, 'Infinity' in ta, Object.keys(ta).join(','));

// A 16-bit string equal to a builtin name must reach the same property.
const wide = ('lengthĀ').slice(0, 6);
p('wide', wide === 'length', [1, 2, 3][wide], ({ length: 9 })[wide]);

// App objects with builtin-named keys: JSON, for-in, keys, computed access.
const o = { name: 'n', value: 1, length: 2, then: undefined, toString() { return 'o'; } };
p('json', JSON.stringify(o));
const seen = []; for (const k in o) seen.push(k); p('for-in', seen.join(','));
p('keys', Object.keys(o).join(','), o['na' + 'me'], o[['val', 'ue'].join('')]);

// Error messages quote property names.
try { null.subarray; } catch (e) { p('error', e.message); }
try { undefined.map(); } catch (e) { p('error', e.message); }

// Names built at run time, then used as keys and as atoms again.
const m = new Map([['push', 1]]);
p('map', m.get('pu' + 'sh'), [].hasOwnProperty('pu' + 'sh'), 'push' in []);
p('concat', 'con' + 'structor' in Object.prototype, Object.prototype['con' + 'structor'] === Object);

print(out.join('\n'));
