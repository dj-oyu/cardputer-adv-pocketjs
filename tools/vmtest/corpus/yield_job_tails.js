// Completion tails may execute user code through a custom Promise species.
// Async handlers return a promise before their first stretch finishes.
const order = [];
function work() { let n = 0; for (let i = 0; i < 12; i++) n += i; return n; }
class Derived extends Promise {
  static get [Symbol.species]() {
    return class extends Promise {
      constructor(executor) {
        super((resolve, reject) => executor(
          value => { order.push('species:' + work()); resolve(value); }, reject));
      }
    };
  }
}
const a = Derived.resolve(3).then(async value => {
  order.push('handler:' + work());
  await 0;
  return value + 1;
});
const b = Promise.resolve({ then(resolve) {
  order.push('thenable:' + work());
  resolve(9);
  throw Error('ignored after resolve');
}});
const c = Promise.resolve({ then() { work(); throw Error('rejected'); } })
  .catch(e => e.message);
queueMicrotask(() => order.push('microtask'));
Promise.all([a, b, c]).then(values => {
  print('values', values.join(','));
  print('order', order.join(','));
});
async function* gen() {
  try { await 0; yield work(); await 0; throw Error('generator'); }
  finally { print('finally', work()); }
}
const iterator = gen();
const first = iterator.next();
const second = iterator.next().catch(e => e.message);
const third = iterator.next();
Promise.all([first, second, third]).then(values =>
  print('generator', JSON.stringify(values)));
