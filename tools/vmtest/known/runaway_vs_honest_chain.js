// NOT a corpus file: this is the demonstration for a DEFECT in the shipping
// L1 constants, and its result depends on the budget, which is exactly why it
// cannot live in corpus/ (run.sh re-runs every corpus file at several budgets
// and demands the same bytes).
//
//   ../../.cache/vmtest/vmrun-o2 --profile host --runaway-turns 30 \
//       --budget-jobs 64 tools/vmtest/known/runaway_vs_honest_chain.js
//
// The firmware ships VM_RUNAWAY_TURNS = 30 and VM_JOB_BACKSTOP = 64. The
// backstop is a HARD ceiling: a continuation turn ends at 64 jobs even when
// those 64 jobs cost far less than the 8 ms time budget, which is the measured
// (device) case for the Promise-chain workload (D, 0.07 ms/job -- 64 jobs is
// 4.5 ms). So a drain that is merely LONG, and making perfect forward
// progress, is killed after 30 x 64 = 1,920 jobs.
//
// docs/vm-L1-design.md sec.5.2 justifies 30 as "30 x 8 ms = 240 ms of JS time,
// about what the old 250 ms wall-clock guard allowed". That equivalence only
// holds when the CLOCK ends each turn. When the backstop ends it, the counter
// stops measuring time at all: at 0.07 ms/job the new guard allows 134 ms of
// JS where the old one allowed 250 ms -- it is roughly twice as strict as the
// guard it replaces, for the one workload whose measurement the design cites.
//
// tools/vmtest/corpus/promise_chain.js (a 3,000-deep then chain) is already
// such a program and exits 5 at --budget-jobs 8, 16 and 64 with
// --runaway-turns 30. The corpus does not notice because vmrun's
// --runaway-turns defaults to OFF.
//
// Expected today: exit 5, no "done" line. A fix belongs to the design (what
// total JS time may one logical drain spend), not to this file.
print('start');
let p = Promise.resolve();
for (let i = 0; i < 2500; i++) p = p.then(() => {});
p.then(() => print('done: 2500 honest jobs finished'));
