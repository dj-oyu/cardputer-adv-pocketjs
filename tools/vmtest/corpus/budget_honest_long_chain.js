// vmrun-flags: --runaway-jobs 100000
// L1 sec.5.2, the regression that the FIRST runaway guard was: a long chain
// that is making perfect forward progress must finish, whatever the budget did
// to it on the way.
//
// The guard used to count consecutive continuation turns (30). A turn ends
// either because the wall clock says the budget is spent -- which on a
// contended board is mostly somebody else's time -- or because the 64-job
// backstop says so however cheap the jobs were. Neither number measures the
// guest's appetite, and 30 x 64 meant that any drain past 1,920 cheap jobs was
// declared a runaway: at the measured (device) 0.07 ms a job that is 134 ms of
// JavaScript, where the 250 ms wall-clock guard it replaced would have let the
// chain finish. This file was the demonstration (it lived in known/ as
// runaway_vs_honest_chain.js and exited 5 at --budget-jobs 8, 16 and 64).
//
// The guard now totals what the drain spent -- microseconds inside the drain,
// with a job total as the dead-clock fallback -- so this chain is only long,
// and "done" is printed at every budget the corpus runs. The flag above is the
// firmware's own VM_RUNAWAY_JOBS, unaltered: the point is that the shipping
// constant does not condemn this program.
print('start');
let p = Promise.resolve();
for (let i = 0; i < 2500; i++) p = p.then(() => {});
p.then(() => print('done: 2500 honest jobs finished'));
