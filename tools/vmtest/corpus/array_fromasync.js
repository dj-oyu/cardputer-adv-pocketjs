// vmrun-flags: --harden
// docs/vm/c-builtins-zip-fromasync.md: Array.fromAsync in C. Same output on
// asan and asan-rom, and under --force-yield / --budget-jobs: every await is
// a job boundary.
const out = [];
function t(name, p) {
  out.push(p.then(v => name + ' ok ' + JSON.stringify(v), e => name + ' ' + e.name + ' ' + e.message));
}
function* gen(log, vals) {
  try { for (const v of vals) { log.push('yield ' + String(v)); yield v; } }
  finally { log.push('gen finally'); }
}
async function* agen(log, vals) {
  try { for (const v of vals) { log.push('ayield ' + v); yield v; } }
  finally { log.push('agen finally'); }
}

print('typeof', typeof Array.fromAsync, Array.fromAsync.length, Array.fromAsync.name);
t('sync', Array.fromAsync([1, Promise.resolve(2), 3]));
t('mapfn', Array.fromAsync([1, 2, 3], async (x, i) => x * 10 + i));
t('async-gen', Array.fromAsync(agen([], [4, 5])));
t('array-like', Array.fromAsync({length: 3, 0: 'a', 1: Promise.resolve('b'), 2: 'c'}));
t('array-like-holes', Array.fromAsync({length: 2}));
t('bad-mapfn', Array.fromAsync([], 42));
t('null', Array.fromAsync(null));
t('this-ctor', Array.fromAsync.call(function C() { this.made = true; }, [1]).then(a => [a.made, a.length, a[0]]));
t('returns-promise', Promise.resolve(Array.fromAsync([]) instanceof Promise));

// mapfn throws: return() runs and settles before the rejection
{
  const log = [];
  const it = {
    [Symbol.asyncIterator]() { return this; },
    next() { log.push('next'); return Promise.resolve({value: 1, done: false}); },
    return() { log.push('return called'); return Promise.resolve().then(() => { log.push('return settled'); return {}; }); },
  };
  t('mapfn-throws-closes', Array.fromAsync(it, () => { log.push('mapfn'); throw new Error('m'); }).catch(e => { log.push('rejected ' + e.message); return log; }));
}
// next() rejecting: the iterator is broken, not closed
{
  const log = [];
  const it = {
    [Symbol.asyncIterator]() { return this; },
    next() { return Promise.reject(new Error('n')); },
    return() { log.push('return called'); return {}; },
  };
  t('next-rejects-no-close', Array.fromAsync(it).catch(e => { log.push('rejected ' + e.message); return log; }));
}
// a sync iterable whose value rejects: the generator is closed (closeOnRejection)
{
  const log = [];
  t('sync-reject-closes', Array.fromAsync(gen(log, [1, {then(_, rej) { rej(new Error('v')); }}, 3])).catch(e => { log.push('rejected ' + e.message); return log; }));
}
// normal completion does not call return()
{
  const log = [];
  t('done-no-return', Array.fromAsync(gen(log, [1, 2])).then(a => { log.push('got ' + a); return log; }));
}
// many awaits: the state machine and its continuations do not leak
t('many', Array.fromAsync({length: 2000}, (_, i) => i).then(a => a.reduce((s, x) => s + x, 0)));

Promise.all(out).then(lines => lines.forEach(l => print(l)));
