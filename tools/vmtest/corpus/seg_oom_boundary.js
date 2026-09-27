// vmrun-flags: --profile device
// L2a corpus (docs/vm-L2-design.md sec.1.1 #6, "OOM right at a segment
// boundary"): a FIXED, small recursion depth (well under the device's
// ~16-29 level C-stack ceiling that deep_recursion_device.js maps), where one
// of the levels makes a single large request that alone exceeds the 160 KiB
// JS_SetMemoryLimit -- same shape as memory_device.js's OOMs (one big
// request with headroom to spare), but here it fires while a short chain of
// ordinary call frames is still on the stack rather than at top level. Once
// frames live in segments, that is exactly the moment a segment boundary can
// be crossed. Deliberately NOT a creeping-up-on-the-limit OOM: README.md's
// known/oom_backtrace_uaf.js documents an upstream use-after-free in
// build_backtrace that a creeping OOM can trigger, and a fragile repro
// (breaks if a byte of source changes) is not what this file is for.
// Not `new Array(1 << 14).fill(depth)`, which this file used until backlog
// #15: it grows 1.5x at a time and crept up to 162,500 of 163,840 B (host,
// device profile) before the growth that failed, so the InternalError had
// ~1.3 KB to be built in, and the second attempt -- which still holds the
// first error in the catch binding -- came out `null` under the -yield/-tco/
// -lazy -keepsrc builds. apply() asks for its array in one request, as in
// memory_device.js.
function pileOnce(depth, out) {
  if (depth === 2) out.push(Array.apply(null, { length: 60000 })); // one big block, mid-chain
  else out.push(depth);
  if (depth === 0) return out.length;
  return pileOnce(depth - 1, out);
}
let kind = "none";
try { pileOnce(5, []); } catch (e) { kind = e === null ? "null" : e.constructor.name; }
print("oom-kind", kind);

// Immediately again: must not still be broken from the first OOM (a UAF or a
// half-freed segment from the first run would most likely show up here).
let kind2 = "none";
try { pileOnce(5, []); } catch (e) { kind2 = e === null ? "null" : e.constructor.name; }
print("oom-kind-again", kind2);

// Ordinary, shallow recursion with no per-level allocation must still work --
// confirms the call machinery itself, not just the allocator, survived.
function sum(n) { return n === 0 ? 0 : n + sum(n - 1); }
print("after-oom-recursion", sum(5));
print("after-oom-basic", [1, 2, 3].map((x) => x * 2).join());
