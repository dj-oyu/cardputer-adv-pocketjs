// vmrun-flags: --budget-jobs 4 --stop-turns 25
// L1 sec.3.2, the hard version of stop_with_queue.js. That file tears the
// session down with a flat then-chain queued and one tracked rejection. This
// one tears it down while the queue holds every shape that owns something the
// runtime has to free without running it:
//
//   - async functions suspended at an await, whose frames hold closures over
//     live objects (the pre-L1 note in ledger 03 fact 39-41 is that an
//     INTERRUPT strands these forever; a stop at a job boundary must merely
//     free them);
//   - try/finally blocks that will never reach their finally, because the job
//     that would resume them is discarded -- "NEVER" must not print;
//   - a generator suspended inside try/finally, and an async generator with an
//     outstanding request in its queue;
//   - a thenable whose then() is itself a queued job behind the cut;
//   - two rejections still on the tracker's list, one of which HAS a catch
//     queued behind the cut. Neither may be reported: the report point is an
//     empty queue (sec.3.1), and this queue never empties.
//
// Pass = exactly the two print lines below, exit 0, "#info jobs_dropped=1",
// and a clean LSan report from the asan build. Any "Unhandled Promise
// rejection" line, any "NEVER", or any leak is a failure.
print('start');

const held = { payload: new Array(64).fill('x') };

async function suspended(tag) {
  try {
    await barrier;
    print('NEVER: past the await ' + tag);
  } finally {
    print('NEVER: finally ' + tag);
  }
}

function* gen() {
  try { yield 1; yield 2; } finally { print('NEVER: generator finally'); }
}
const g = gen();
g.next();

async function* agen() { yield held; yield 1; }
const ag = agen();
ag.next();
ag.next();

// The barrier resolves only at the far end of a chain longer than the session
// lives, so every await above is still suspended when the runtime goes.
let tail = Promise.resolve();
for (let i = 0; i < 4000; i++) tail = tail.then(() => held);
const barrier = tail;

for (let i = 0; i < 4; i++) suspended(i);

// Hung off the barrier so the thenable's own then() is a job behind the cut.
const thenable = { then(res) { print('NEVER: thenable then() ran'); res(held); } };
tail.then(() => thenable).then(() => print('NEVER: thenable settled'));

const caughtLater = Promise.reject(new Error('catch-was-in-a-dropped-job'));
tail.then(() => caughtLater.catch(() => print('NEVER: late catch')));
Promise.reject(new Error('never-had-a-catch'));

print('queued');
