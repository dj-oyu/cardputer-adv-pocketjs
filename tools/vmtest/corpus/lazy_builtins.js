// F2 (docs/vm/builtin-floor-plan.md sec.5.2): builtins whose methods stay in
// flash lists until touched must look exactly like eager ones -- same keys,
// same order, same descriptors, same identities -- whatever was touched,
// deleted or redefined first. The expected file is made by the eager build.
// First, while everything is still cold: an assignment has to find a setter
// or a read-only property that is still pending up the prototype chain.
const cold = {}; cold.__proto__ = Array.prototype;
const coldM = Object.create(Math); coldM.PI = 1;
const coldLine = ['cold-set', Array.isArray(cold), cold instanceof Array,
  Object.getPrototypeOf(cold) === Array.prototype, coldM.PI === Math.PI,
  Object.prototype.hasOwnProperty.call(coldM, 'PI')].join(' ');
// ...and a delete of a pending entry has to stick, before anything lists
// the object (listing puts every pending entry in the shape first).
const coldDel = ['cold-delete', delete Date.prototype.getYear, 'getYear' in Date.prototype,
  delete Math.LN2, 'LN2' in Math, delete Reflect.isExtensible, typeof Reflect.isExtensible].join(' ');
const out = [coldLine, coldDel];
const p = (...a) => out.push(a.join(' '));
const keys = o => Reflect.ownKeys(o).map(String).join(',');
const desc = (o, k) => {
  const d = Object.getOwnPropertyDescriptor(o, k);
  if (!d) return 'none';
  return (d.get ? 'get' : '') + (d.set ? 'set' : '') + ('value' in d ? typeof d.value : '') +
    (d.writable ? 'W' : '') + (d.enumerable ? 'E' : '') + (d.configurable ? 'C' : '');
};

// Touch out of order first, then enumerate: definition order must hold.
[].map; Math.max; Object.prototype.toString; new Map().size; 'x'.padEnd;
p('Array.prototype', keys(Array.prototype));
p('Math', keys(Math));
p('Object.prototype', keys(Object.prototype));
p('Map.prototype', keys(Map.prototype));
p('String.prototype', keys(String.prototype));

// Untouched objects, enumerated cold.
for (const [n, o] of [['Array', Array], ['Object', Object], ['Number', Number],
    ['Promise', Promise], ['Reflect', Reflect], ['JSON', JSON], ['Date.prototype', Date.prototype],
    ['RegExp.prototype', RegExp.prototype], ['Set.prototype', Set.prototype],
    ['Symbol', Symbol], ['Function.prototype', Function.prototype],
    ['Error.prototype', Error.prototype], ['Iterator.prototype', Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()))]])
  p(n, keys(o));

// Descriptors of pending entries: method, accessor, constant, symbol-keyed.
p('desc', desc(Array.prototype, 'flat'), desc(Map.prototype, 'size'), desc(Math, 'PI'),
  desc(Number, 'MAX_SAFE_INTEGER'), desc(Array.prototype, Symbol.iterator),
  desc(Function.prototype, Symbol.hasInstance), desc(Symbol.prototype, Symbol.toPrimitive));

// Identity: touched twice is the same function; aliases are their targets.
p('identity', [].indexOf === Array.prototype.indexOf,
  Array.prototype[Symbol.iterator] === Array.prototype.values,
  Set.prototype.keys === Set.prototype.values, String.prototype.trimLeft === String.prototype.trimStart);

// has / in / hasOwnProperty on pending entries, and on names not in any list.
p('has', Object.prototype.hasOwnProperty.call(Math, 'hypot'), 'sign' in Math, 'nope' in Math,
  Reflect.has(Date.prototype, 'toISOString'), Object.prototype.hasOwnProperty.call(Math, 'nope'));

// delete: a pending entry, then it must stay gone; a non-configurable one.
p('delete', delete Math.cbrt, 'cbrt' in Math, Math.cbrt, delete Math.PI, Math.PI);
p('after delete', keys(Math));

// delete a plain property that precedes a list, then enumerate.
p('ctor before', keys(Array.prototype.constructor));
delete Array.name;
p('ctor after', keys(Array));

// Assign / redefine pending entries.
const origJoin = Array.prototype.join;
Array.prototype.join = function () { return 'mine'; };
p('assign', [1, 2].join(), keys(Array.prototype) === keys(Array.prototype));
Array.prototype.join = origJoin;
Object.defineProperty(Number.prototype, 'toFixed', { value: () => 'fixed', writable: true, configurable: true });
p('define', (1).toFixed(), desc(Number.prototype, 'toFixed'));

// Frozen and non-extensible builtins still answer for every entry.
Object.preventExtensions(Boolean.prototype);
p('non-extensible', typeof Boolean.prototype.valueOf, Object.isExtensible(Boolean.prototype));
Object.freeze(JSON);
p('frozen', Object.isFrozen(JSON), typeof JSON.parse, desc(JSON, 'stringify'));

// for-in through builtin prototypes, and one with an enumerable addition.
const o = { a: 1 }; const seen = []; for (const k in o) seen.push(k);
Object.prototype.extra = 1; for (const k in o) seen.push(k); delete Object.prototype.extra;
p('for-in', seen.join(','));

// Getters/setters that live in lists.
const re = /a/g;
p('accessors', re.global, re.flags, new Uint8Array(4).byteLength, [1, 2].length);

// Shadowing a pending entry on an instance.
const arr = [3, 1, 2]; arr.sort = () => 'own';
p('shadow', arr.sort(), [3, 1, 2].sort().join());

print(out.join('\n'));
