// L2a corpus (docs/vm-L2-design.md sec.1.1 #6, "boundary crossing"): a single
// frame whose var_buf/arg_buf is far larger than an ordinary frame, built
// from a generated source so this file stays small (CLAUDE.md: source bytes
// cost guest heap). JS_MAX_LOCAL_VARS is 65535 (quickjs.c:222); N below is
// comfortably under it but big enough that, once frames live in fixed-size
// segments, this one frame should need a segment of its own rather than
// sharing one with its neighbours.
const N = 4000;
let decls = "";
for (let i = 0; i < N; i++) decls += "var v" + i + " = " + i + ";\n";
decls += "return v0 + v" + (N >> 1) + " + v" + (N - 1) + ";\n";
const bigLocals = new Function(decls);
print("big-locals", bigLocals());

// Same idea via arguments: a call with N actual arguments grows arg_buf
// instead of var_buf. build_arg_list enforces the same JS_MAX_LOCAL_VARS cap
// (quickjs.c:42909-42914, docs/vm-L2-design.md sec.2's "apply is bounded").
function sumArgs() {
  let s = 0;
  for (let i = 0; i < arguments.length; i++) s += arguments[i];
  return s;
}
const args = [];
for (let i = 0; i < N; i++) args.push(i);
print("big-args", sumArgs.apply(null, args));

// The oversized frame called from, and returning into, an ordinary chain:
// the boundary must be crossed in both directions without disturbing the
// normal-sized frames around it.
function wrap(n) { return n === 0 ? bigLocals() : 1 + wrap(n - 1); }
print("wrapped", wrap(50));

// And an oversized frame that itself calls back into an ordinary deep chain
// before returning (the crossing happens with both frame shapes still live).
function callBigThenDeep() {
  var w0 = 10, w1 = 20, w2 = 30; // small local footprint on this side
  function deep(n) { return n === 0 ? 0 : 1 + deep(n - 1); }
  const d = deep(2000);
  return w0 + w1 + w2 + d + sumArgs.apply(null, args);
}
print("big-then-deep", callBigThenDeep());
