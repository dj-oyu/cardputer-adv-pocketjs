// vmrun-flags: --host-events --budget-jobs 4
// vmrun-pin-budget
// Fair ordering, the case it exists for (docs/vm-L1-report.md sec.10).
//
// A completion is recorded two job boundaries into a 24-job chain. The chain
// is SEQUENTIAL -- each .then enqueues the next, so the queue is never more
// than one job deep -- which makes the question "does JavaScript hear about
// the completion before the chain ends?" and nothing else.
//
//   compat (expected/):       job 1 .. job 24, then completion 1
//   fair   (expected-fair/):  completion 1 lands between two jobs of the chain
//
// The budget is the count mode (--budget-jobs 4), so where it lands is a pure
// function of the program and not of any clock.
const done = host.request(2);
done.then(v => print("completion " + v));
let p = Promise.resolve();
for (let i = 1; i <= 24; i++) p = p.then(() => print("job " + i));
p.then(() => print("chain end"));
