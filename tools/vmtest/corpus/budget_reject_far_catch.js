// vmrun-flags: --frames 2 --budget-jobs 5
// L1 invariant 5 (docs/vm-L1-design.md sec.3.1), pushed further than
// rejections.js pushes it. There the catch for "handled-same-drain" lands two
// jobs after the rejection, so a budget has to fall in a two-job window to
// misreport it. Here the distances are chosen so that a boundary ALWAYS falls
// between the rejection and its catch, at every budget the corpus is run at
// and at the firmware's own ceiling:
//
//   A: catch attached 70 jobs later -- past VM_JOB_BACKSTOP (64), so even a
//      drain that never reads the clock is cut before the catch arrives.
//   B: rejection CREATED deep inside the drain (job 30) and caught 40 jobs
//      after that, so both ends of the window are in continuation turns and
//      neither is in the turn that started the drain.
//   C: the rejection is created and caught inside the chain frame() queues,
//      so the window lives entirely in the frame's own drain.
//
// None of the three may appear as "Unhandled Promise rejection". D is the
// control: nothing ever handles it, so it MUST be reported -- a scheduler that
// silenced reports altogether would pass A-C and fail here.
function chain(n, fn) {
  let p = Promise.resolve();
  for (let i = 0; i < n; i++) p = p.then(() => { if (fn) fn(i); });
  return p;
}

const a = Promise.reject(new Error('A-far-catch'));
chain(70).then(() => a.catch((e) => print('caught ' + e.message)));

let b = null;
chain(30, (i) => { if (i === 29) b = Promise.reject(new Error('B-mid-drain')); })
  .then(() => chain(40))
  .then(() => b.catch((e) => print('caught ' + e.message)));

Promise.reject(new Error('D-never-handled'));

let n = 0;
globalThis.frame = () => {
  print('frame ' + (++n));
  if (n !== 1) return;
  const c = Promise.reject(new Error('C-inside-frame'));
  chain(70).then(() => c.catch((e) => print('caught ' + e.message)));
};
print('sync-end');
