// vmrun-flags: --profile device
// Refcount-freed churn and OOM under the device's 160 KiB JS_SetMemoryLimit.
// Host blocks are larger (64-bit), so this runs with less headroom than the
// device; the assertions are about behaviour (acyclic garbage is freed at
// once, a failed allocation is a catchable error the runtime survives), never
// about sizes. Cyclic garbage is gc_threshold_device.js. OOM reached by
// creeping up on the limit is known/oom_backtrace_uaf.js (an upstream UAF),
// so the OOMs here are single large requests that leave headroom behind.

// Acyclic garbage: freed by refcount the moment it is dropped, no GC needed.
let sum = 0;
for (let i = 0; i < 20000; i++) {
  const o = { i, arr: [i, i + 1, i + 2], s: "k" + (i % 97) };
  sum = (sum + o.arr[2] + o.s.length) % 1000003;
}
print("churn", sum);

function tryBig(label, f) {
  try { f(); print(label, "no-oom"); }
  catch (e) { print(label, e === null ? "null" : e.constructor.name + ": " + e.message); }
}
tryBig("string-oom", () => "x".repeat(1 << 20));
tryBig("array-oom", () => new Array(1 << 18).fill(0));
tryBig("buffer-oom", () => new Uint8Array(1 << 20));

// And the runtime works after them.
const after = [];
for (let i = 0; i < 100; i++) after.push({ i });
print("after-oom", after.length, JSON.stringify(after[99]));
print("done", [1, 2, 3].map((x) => x * 2).join());
