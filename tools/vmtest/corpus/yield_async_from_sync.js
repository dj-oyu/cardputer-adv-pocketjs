// Native AsyncFromSyncIterator tails must finish before any parked JS resumes.
const events = [];
function mark(s) {
  let n = 0;
  for (let i = 0; i < 7; i++) n += i;
  events.push(s + ':' + n);
}
function iterable(mode) {
  let n = 0;
  return {
    [Symbol.iterator]() { mark('iterator'); return this; },
    get next() {
      mark('get-next');
      return function () {
        mark('next-' + n);
        if (mode === 'next-throw') throw Error('next');
        const value = ++n;
        return {
          get done() { mark('done-' + value); return value > 2; },
          get value() {
            mark('value-' + value);
            if (mode === 'value-throw') throw Error('value');
            return {
              get then() {
                mark('get-then-' + value);
                return (resolve, reject) => {
                  mark('then-' + value);
                  if (mode === 'reject') reject(Error('rejected'));
                  else resolve(value * 10);
                };
              }
            };
          }
        };
      };
    },
    get return() {
      mark('get-return');
      return function (value) {
        mark('return-' + value);
        if (mode === 'return-primitive') return 42;
        return { done: true, value: Promise.resolve('closed') };
      };
    }
  };
}
async function consume(mode) {
  events.length = 0;
  try {
    await 0;
    for await (const value of iterable(mode)) {
      mark('body-' + value);
      if (mode === 'break' || mode === 'return-primitive') break;
    }
    mark('complete');
  } catch (e) {
    mark('caught-' + (e instanceof TypeError ? 'TypeError' : e.message));
  } finally {
    mark('finally');
  }
  print(mode, events.join(','));
}
async function delegate(mode) {
  events.length = 0;
  async function* gen() {
    try { await 0; return yield* iterable(mode); }
    finally { mark('delegate-finally'); }
  }
  const g = gen();
  const first = await g.next();
  mark('first-' + first.value);
  try {
    const last = mode === 'throw' ? await g.throw(Error('outer')) : await g.return(99);
    mark('last-' + last.value + '-' + last.done);
  } catch (e) { mark('delegate-caught-' + e.name); }
  print('delegate-' + mode, events.join(','));
}
(async () => {
  for (const mode of ['normal', 'break', 'next-throw', 'value-throw', 'reject', 'return-primitive'])
    await consume(mode);
  await delegate('return');
  await delegate('throw');
  print('async-from-sync-complete');
})();
