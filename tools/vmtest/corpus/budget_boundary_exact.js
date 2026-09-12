// vmrun-flags: --frames 2 --budget-jobs 6 --host-events
// vmrun-pin-budget
// L1 invariant 6 (docs/vm-L1-design.md sec.2.1, sec.7) at the one place
// budget_completions.js does not look: the boundary ITSELF.
//
// budget_completions.js records eight completions from the top level, all
// falling due somewhere in the middle of a long continuation run. The cases
// that can actually break the rule are the edges, so this file asks for:
//
//   E0  requested from INSIDE a job, with k=0 -- it becomes ready at the very
//       boundary the drain is about to reach. It must still wait for the queue
//       to empty, not be delivered at the boundary it was born on.
//   E1  requested from inside a completion HANDLER (re-entrant): the host is
//       asked for more work while it is delivering work. Its reaction queues a
//       fresh 30-job chain, and
//   E2  is requested from inside that chain -- so a completion is recorded
//       while a drain that a completion started is still running. It must land
//       after that chain ends, never inside it.
//
// The order that proves it: every "in-chain" line of a chain precedes the
// "got" line of any completion recorded during it, and each "got" appears
// exactly once with a strictly increasing host sequence number.
const out = [];
function chain(n, tag) {
  let p = Promise.resolve();
  for (let i = 0; i < n; i++)
    p = p.then(() => { if (i === n - 1) out.push('in-chain ' + tag); });
  return p;
}

// The queue is busy for ~60 jobs; E0 is requested from job 20 of it with k=0.
chain(20, 'pre').then(() => {
  host.request(0).then((s) => out.push('got E0 seq=' + s));
  return chain(40, 'post');
});

host.request(1).then((s) => {
  out.push('got E1 seq=' + s);
  return chain(30, 'E1-body').then(() => {
    host.request(0).then((t) => out.push('got E2 seq=' + t));
  });
});

let n = 0;
globalThis.frame = () => {
  out.push('frame ' + (++n));
  if (n === 2) for (const line of out) print(line);
};
