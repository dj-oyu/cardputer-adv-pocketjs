// L2a corpus (docs/vm-L2-design.md sec.1.1 #6, "return and reuse"): dive and
// unwind many separate times, so that once segments exist a returned segment
// gets handed back to a LATER, unrelated call chain. Each cycle stamps its
// frames with a value that encodes both the cycle number and the depth, so a
// segment returned dirty (not cleared, not reallocated) and read back by a
// later cycle shows up as the wrong stamp rather than as a crash.
function dive(n, cycle) {
  const stamp = cycle * 100000 + n;   // unique across cycles AND depths
  if (n === 0) return stamp;
  const below = dive(n - 1, cycle);
  if (below !== cycle * 100000 + (n - 1)) return -1;   // corruption sentinel
  return stamp;
}
let bad = 0;
for (let c = 0; c < 40; c++) {
  const r = dive(800, c);
  if (r !== c * 100000 + 800) bad++;   // dive returns the TOP level's stamp
}
print("reuse-cycles-bad", bad);

// Same shape but with a large per-frame array, so a reused segment carrying
// a stale object reference (rather than a stale scalar) would leave a
// detectable trace: each cycle fills its arrays with its own cycle id and
// checks every one back after the whole cycle has unwound.
function fillDive(n, cycle, bag) {
  bag.push(cycle);
  if (n === 0) return;
  fillDive(n - 1, cycle, bag);
}
let mismatches = 0;
for (let c = 0; c < 20; c++) {
  const bag = [];
  fillDive(300, c, bag);
  for (const v of bag) if (v !== c) mismatches++;
}
print("fill-mismatches", mismatches);
