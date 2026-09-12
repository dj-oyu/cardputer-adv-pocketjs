// L2a corpus (docs/vm-L2-design.md sec.1.1 #6, "segment addition"): deep but
// non-overflowing recursion, well past the size of any single segment the
// design is likely to pick (sec.3.2's KB-scale distribution), with per-level
// state that only reads back correctly if a frame further down the chain
// did not clobber it. deep_recursion.js already fixes overflow BEHAVIOUR
// (catchable, recoverable); this fixes VALUES surviving segment growth.
function sumDown(n, acc) {
  if (n === 0) return acc;
  const mark = n * 3 + 1;              // distinct per level, read AFTER the recursive call
  const r = sumDown(n - 1, acc + n);
  return r + (mark - (n * 3 + 1));     // must be r + 0: mark must survive untouched
}
print("deep-sum", sumDown(6000, 0));

// Mutual recursion of the same depth: two different function objects and
// call sites per level, so a segment bug tied to one callee shape would not
// hide behind the other.
function pingArr(n, arr) { arr.push(n); return n === 0 ? arr : pongArr(n - 1, arr); }
function pongArr(n, arr) { arr.push(-n); return n === 0 ? arr : pingArr(n - 1, arr); }
const trail = pingArr(3000, []);
print("trail", trail.length, trail[0], trail[1], trail[trail.length - 1]);
