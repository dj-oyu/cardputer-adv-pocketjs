// vmrun-flags: --host-events --budget-jobs 2
// vmrun-pin-budget
// Fair ordering must not reorder the job queue, only interleave host events
// with it. This is the proof, and it needs a queue more than one job deep.
//
// Four independent two-step chains: the queue starts [a1 a2 a3 a4], and each
// a-job enqueues its b-job at the TAIL as it runs. A completion is recorded at
// boundary 1, i.e. after a1 and a2 have run and b1 and b2 are already queued.
//
// Fair ordering settles it there, by calling a resolve function -- which
// APPENDS (ledger 03 fact 53). So the completion's handler must run after b2
// (queued before it) and before b3 (queued after it). If it ever appears
// earlier than that, something is putting host work at the HEAD of the queue
// and the FIFO the pre-L1 drain guaranteed is gone.
//
//   compat: a1 a2 a3 a4 b1 b2 b3 b4 completion 1
//   fair:   a1 a2 a3 a4 b1 b2 completion 1 b3 b4
host.request(1).then(v => print("completion " + v));
for (let i = 1; i <= 4; i++)
  Promise.resolve()
    .then(() => print("a" + i))
    .then(() => print("b" + i));
