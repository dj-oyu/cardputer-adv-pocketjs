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
// Not `new Array(1 << 18).fill(0)`: that grows the array 1.5x at a time and
// creeps up on the limit before the growth that fails (measured on the host,
// device profile: 163,452 of 163,840 B charged at the rejection), so whether
// the InternalError could still be allocated was a byte-margin coincidence;
// the charged sizes moving with backlog #9 turned it into `null`. apply()
// builds its argument array in one request (host: 960,000 B).
tryBig("array-oom", () => Array.apply(null, { length: 60000 }));
tryBig("buffer-oom", () => new Uint8Array(1 << 20));

// And the runtime works after them.
const after = [];
for (let i = 0; i < 100; i++) after.push({ i });
print("after-oom", after.length, JSON.stringify(after[99]));
print("done", [1, 2, 3].map((x) => x * 2).join());
