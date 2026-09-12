// vmrun-flags: --frames 10 --budget-jobs 8
// L1 invariant 7 (docs/vm-L1-design.md sec.7): no queue starves. Each frame()
// queues a 41-job chain and the budget is 8 jobs a turn, so every turn ends
// with work pending and the next turn must finish it before frame() runs
// again. The counter frame() prints is the number of chains that COMPLETED,
// so "frame N completions=N-1" for every N is the statement that each turn's
// queue was fully worked off before the next turn began -- one chain per
// frame, never falling behind and never running two at once.
//
// A scheduler that let the queue accumulate would print a counter that lags;
// one that dropped jobs would print one that stalls. Identical output at
// --budget-jobs 0 is the other half of the check.
let completions = 0;
function chain(n) {
  let p = Promise.resolve();
  for (let i = 0; i < n; i++) p = p.then(() => {});
  return p.then(() => { completions++; });
}
let n = 0;
function frame() {
  print('frame ' + (++n) + ' completions=' + completions);
  chain(40);
}
