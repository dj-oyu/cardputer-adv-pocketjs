// vmrun-flags: --frames 3 --budget-jobs 8
// L1 invariant 3 (docs/vm-L1-design.md sec.7): the budget may cut a drain, but
// frame() must never land INSIDE one. A turn that yields with jobs queued
// finishes them at the top of the next turn before any host call reaches JS,
// so from the program's side the queue still empties exactly once between two
// frames -- whatever the budget is.
//
// Read the expected file as a fence: every "step" line belongs to the chain
// built at evaluation and must be complete before "frame 1"; every "t" line
// belongs to the chain frame 1 builds and must be complete before "frame 2".
// A budget that reordered these would put a frame line in the middle.
//
// The same output must come out at --budget-jobs 0 (no budget at all), which
// is what makes this a test rather than a recording.
let step = 0;
let p = Promise.resolve();
for (let i = 0; i < 100; i++) p = p.then(() => { if (++step % 25 === 0) print('step ' + step); });

let n = 0;
function frame() {
  print('frame ' + (++n));
  if (n !== 1) return;
  let q = Promise.resolve();
  for (let i = 0; i < 50; i++) q = q.then(() => { if (i % 25 === 0) print('t ' + i); });
}
