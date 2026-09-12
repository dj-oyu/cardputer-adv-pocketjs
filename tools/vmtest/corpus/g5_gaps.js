// vmrun-flags: --gaps
// G5 (docs/vm-L2-design.md sec.1.3): does the gap recorder see the sections
// sec.2 lists as unreachable by opcode checkpoints? One named function per
// section, each opening with a trivial branch so the section's gap STARTS at a
// safepoint inside it (start=safepoint:<name> in "#info g5 top[k]") and ends
// at the caller's loop branch. The printed values are the diffed contract;
// the "#info g5" lines are the measurement and are not diffed.
function n1_regex() {
  if (n1_regex.length) return;
  // Exponential backtracking: 2^18 attempts, no bytecode in between.
  return /^(a+)+b$/.test("a".repeat(18));
}
function n2_native_json() {
  if (n2_native_json.length) return;
  return JSON.stringify(shared).length;
}
function n2_native_sort_callback() {
  if (n2_native_sort_callback.length) return;
  // Every comparator call is a prologue poll (N5), none is a safepoint: the
  // whole sort is one gap although it runs JS the whole time.
  const a = shared.slice();
  a.sort((x, y) => x.n - y.n);
  return a[0].n + "," + a[a.length - 1].n;
}
function n3_forin() {
  if (n3_forin.length) return;
  // One for_in_next step skips every non-enumerable property (N3).
  let count = 0;
  for (const k in sparse) count++;
  return count;
}
function n4_direct_eval() {
  if (n4_direct_eval.length) return;
  // Parse of a long straight-line source, then a run with no branches.
  return eval(straight);
}
function n5_prologues() {
  if (n5_prologues.length) return;
  return c0();
}
function c0() { return c1() + 1; }
function c1() { return c2() + 1; }
function c2() { return c3() + 1; }
function c3() { return c4() + 1; }
function c4() { return c5() + 1; }
function c5() { return c6() + 1; }
function c6() { return c7() + 1; }
function c7() { return 0; }

const shared = [];
for (let i = 0; i < 20000; i++) shared.push({ n: (i * 7919) % 20011, s: "v" + i });
const sparse = {};
for (let i = 0; i < 3000; i++) Object.defineProperty(sparse, "p" + i, { value: i, enumerable: false });
sparse.first = 1;
sparse.last = 2;
let straight = "var x = 0;";
for (let i = 0; i < 20000; i++) straight += "x = x + 1;";
straight += "x";

for (const s of [n1_regex, n2_native_json, n2_native_sort_callback, n3_forin, n4_direct_eval, n5_prologues])
  print(s.name, s());
