// vmrun-flags: --host-events --budget-jobs 3
// vmrun-pin-budget
// The unhandled-rejection report point is NOT a thing fair ordering moves.
//
// A completion delivered in the middle of a drain rejects a promise whose
// catch is attached 8 jobs later, still inside the same logical drain. The
// report point is where vm_sched_drain() returned EMPTY and nowhere else
// (vm-L1-design sec.3.1), so in BOTH modes this must report nothing -- only
// the one rejection nobody ever handles is reported, and only at the end.
//
// The two modes DO interleave the two chains differently -- that is fair
// ordering doing its job, and it is why this file has an expected file per
// mode -- but both must end with exactly one report, for the rejection nobody
// handles, and neither may report the one that is caught eight jobs later.
// This file fails if fair ordering turned the boundary it added into a
// reporting point.
let late = null;
host.request(1).then(() => {
  late = Promise.reject(new Error("caught eight jobs later"));
  let p = Promise.resolve();
  for (let i = 1; i <= 8; i++) p = p.then(() => print("after " + i));
  p.then(() => late.catch(e => print("caught: " + e.message)));
});
let q = Promise.resolve();
for (let i = 1; i <= 12; i++) q = q.then(() => print("chain " + i));
q.then(() => { Promise.reject(new Error("nobody catches this")); });
