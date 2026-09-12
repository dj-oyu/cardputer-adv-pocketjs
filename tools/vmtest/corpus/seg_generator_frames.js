// L2a corpus (docs/vm-L2-design.md sec.1.1 #6): generator/async frames use a
// different suspend path than an alloca'd call frame (sec.4.2's
// done_generator), and their JSStackFrame lives inside a JSAsyncFunctionState
// rather than on the C stack (docs/vm-ledger/02-frame-pointers.md). Interleave
// resumption with unrelated deep recursion between every step, so a segment
// that recursion returns cannot be mistaken for the suspended frame's own
// storage when the generator resumes.
function churn(n) { return n === 0 ? 0 : 1 + churn(n - 1); }

function* g(n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    total += i;
    yield total;
  }
  return total;
}
const it = g(6);
const seen = [];
for (let r = it.next(); ; r = it.next()) {
  seen.push(r.value);
  churn(1500);           // deep, unrelated recursion between every resume
  if (r.done) break;
}
print("gen-seen", seen.join(","));

// Two generators interleaved (round-robin), each captured over its own
// closed-over counter, with churn between EVERY step of EITHER one: a
// segment reused for A's suspended frame must never answer for B's.
function* counter(tag, start) {
  let v = start;
  while (true) { v += 1; yield tag + ":" + v; }
}
const a = counter("A", 0), b = counter("B", 100);
const rr = [];
for (let i = 0; i < 10; i++) {
  rr.push(a.next().value);
  churn(800);
  rr.push(b.next().value);
  churn(800);
}
print("round-robin", rr.join(","));

// Async/await: every await resumes on a later microtask turn, by which time
// unrelated JS (churn) has already run and returned deeply in between.
async function chain(n) {
  let acc = 0;
  for (let i = 0; i < n; i++) {
    acc += await Promise.resolve(i);
    churn(1000);
  }
  return acc;
}
chain(6).then((v) => print("async-chain", v));
