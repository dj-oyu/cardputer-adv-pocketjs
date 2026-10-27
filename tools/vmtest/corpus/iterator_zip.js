// vmrun-flags: --harden
// docs/vm/c-builtins-zip-fromasync.md: Iterator.zip / zipKeyed in C. Same
// output on asan and asan-rom (the ROM build used to have neither).
function t(name, f) {
  try { print(name, 'ok', String(f())); }
  catch (e) { print(name, e.name, e.message); }
}
function logged(name, vals, log, opts) {
  let i = 0;
  return {
    [Symbol.iterator]() { return this; },
    next() { log.push(name + '.next'); return i < vals.length ? {value: vals[i++], done: false} : {value: undefined, done: true}; },
    return() { log.push(name + '.return'); if (opts && opts.throwOnReturn) throw new Error(name); return {}; },
  };
}
const arr = it => JSON.stringify([...it]);

t('typeof', () => [typeof Iterator.zip, typeof Iterator.zipKeyed, Iterator.zip.length, Iterator.zip.name].join());
t('proto', () => Object.getPrototypeOf(Iterator.zip([])) === Object.getPrototypeOf([].values().map(x => x)));
t('not-ctor', () => { try { new Iterator.zip([]); return 'constructed'; } catch (e) { return e.name; } });
t('shortest', () => arr(Iterator.zip([[1, 2, 3], ['a', 'b']])));
t('longest', () => arr(Iterator.zip([[1, 2, 3], ['a']], {mode: 'longest', padding: [0, 'z']})));
t('longest-nopad', () => arr(Iterator.zip([[1], [2, 3]], {mode: 'longest'})));
t('strict-ok', () => arr(Iterator.zip([[1, 2], [3, 4]], {mode: 'strict'})));
t('strict-bad', () => arr(Iterator.zip([[1, 2], [3]], {mode: 'strict'})));
t('bad-mode', () => Iterator.zip([], {mode: new String('longest')}));
t('empty', () => arr(Iterator.zip([])));
t('fresh-arrays', () => { const it = Iterator.zip([[1, 2], [3, 4]]); const a = it.next().value, b = it.next().value; return a !== b; });
t('keyed', () => JSON.stringify([...Iterator.zipKeyed({x: [1, 2], y: ['a', 'b'], skip: undefined})]));
t('keyed-proto', () => Object.getPrototypeOf(Iterator.zipKeyed({a: [1]}).next().value) === null);
t('keyed-symbol', () => { const s = Symbol('s'); const v = Iterator.zipKeyed({[s]: [7]}).next().value; return v[s]; });
t('keyed-longest', () => JSON.stringify([...Iterator.zipKeyed({a: [1, 2], b: [3]}, {mode: 'longest', padding: {b: 'B'}})]));

// closing: shortest closes the others in reverse when one is done
t('close-shortest', () => { const log = []; const it = Iterator.zip([logged('a', [1, 2], log), logged('b', [1], log), logged('c', [1, 2], log)]);
  it.next(); it.next(); return log.join(' '); });
// the iterator whose next() throws is not closed; the others are, in reverse
t('close-throw', () => { const log = []; const bad = {next() { log.push('bad.next'); throw new Error('x'); }, return() { log.push('bad.return'); }};
  const it = Iterator.zip([logged('a', [1], log), bad, logged('c', [1], log)]);
  try { it.next(); } catch (e) { log.push('caught ' + e.message); }
  log.push(JSON.stringify(it.next())); return log.join(' '); });
// return() before next(): complete first, then close; the first throw wins
t('return-start', () => { const log = []; const it = Iterator.zip([logged('a', [1], log, {throwOnReturn: true}), logged('b', [1], log, {throwOnReturn: true})]);
  try { it.return(); } catch (e) { log.push('caught ' + e.message); }
  log.push(JSON.stringify(it.next())); return log.join(' '); });
t('return-mid', () => { const log = []; const it = Iterator.zip([logged('a', [1, 2], log), logged('b', [1, 2], log)]);
  it.next(); log.push(JSON.stringify(it.return())); return log.join(' '); });
// re-entering return() while closing a not-yet-started zip
t('return-reenter', () => { let it; const inner = {next() { return {done: true}; }, return() { return {r: JSON.stringify(it.return())}; }};
  it = Iterator.zip([inner]); it.return(); return 'no error'; });
// re-entering next() while running
t('running', () => { let it; const inner = {next() { it.next(); return {done: true}; }};
  it = Iterator.zip([inner]); return JSON.stringify(it.next()); });
t('gc', () => { let n = 0; for (let k = 0; k < 2000; k++) for (const [a, b] of Iterator.zip([[k, 1], [2, 3]])) n += a + b; return n; });
