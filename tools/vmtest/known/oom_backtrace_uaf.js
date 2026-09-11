// vmrun-flags: --profile device
// Refcount-freed churn and OOM under the device's 160 KiB JS_SetMemoryLimit.
// Host blocks are larger (64-bit), so this runs with less headroom than the
// device; the assertions are about behaviour (acyclic garbage is freed at
// once, OOM is catchable and recoverable), never about sizes. Cyclic garbage
// is gc_threshold_device.js.

// Acyclic garbage: freed by refcount the moment it is dropped, no GC needed.
let sum = 0;
for (let i = 0; i < 20000; i++) {
  const o = { i, arr: [i, i + 1, i + 2], s: "k" + (i % 97) };
  sum = (sum + o.arr[2] + o.s.length) % 1000003;
}
print("churn", sum);

// Out of memory with acyclic data: catchable, and dropping the array frees
// everything again.
let hog = [];
let caught = false;
try { for (;;) hog.push(new Array(64).fill(0)); }
catch (e) { caught = true; print("#info oom_value=" + (e === null ? "null" : e.constructor.name)); }
print("#info chunks_before_oom=" + hog.length);
hog = null;
print("oom-caught", caught);

// And the runtime works after it.
const after = [];
for (let i = 0; i < 100; i++) after.push({ i });
print("after-oom", after.length, JSON.stringify(after[99]));

// String building up to the limit, then released.
let s = "";
caught = false;
try { for (let i = 0; i < 1e6; i++) s += "abcdefgh"; } catch (e) { caught = true; }
s = null;
print("string-oom-caught", caught);
print("done", [1, 2, 3].map((x) => x * 2).join());
