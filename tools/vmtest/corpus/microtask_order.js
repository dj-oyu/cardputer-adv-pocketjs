// vmrun-flags: --frames 1
// Microtask ordering: the exact interleaving of then/catch/finally/await,
// thenables and queueMicrotask. Every line is numbered by the order the VM
// runs it; spec L1 section: FIFO and then/catch/finally order must match.
const log = [];
const L = (s) => log.push(s);

L("sync-1");
Promise.resolve().then(() => L("p1-then1")).then(() => L("p1-then2")).then(() => L("p1-then3"));
Promise.resolve().then(() => L("p2-then1")).then(() => L("p2-then2"));
queueMicrotask(() => L("qm-1"));

Promise.reject("r").catch(() => L("catch-1")).finally(() => L("finally-1")).then(() => L("after-finally"));
Promise.resolve("v").finally(() => L("finally-2")).then((v) => L("finally-passes-" + v));
Promise.reject("e").finally(() => L("finally-3")).catch((e) => L("finally-rethrows-" + e));

// finally returning a rejected promise overrides the fulfilment.
Promise.resolve(1).finally(() => Promise.reject("fin-override")).catch((e) => L("override-" + e));

async function af1() { L("af1-start"); await undefined; L("af1-after-await1"); await null; L("af1-after-await2"); }
af1().then(() => L("af1-done"));

async function af2() { L("af2-start"); await Promise.resolve(); L("af2-after-await-promise"); return "x"; }
af2().then((v) => L("af2-done-" + v));

// Await a thenable: one extra tick to call then().
const thenable = { then(res) { L("thenable-then-called"); res("T"); } };
(async () => { const v = await thenable; L("await-thenable-" + v); })();

// Resolve with a thenable / a native promise: NewPromiseResolveThenableJob.
new Promise((res) => res(thenable)).then((v) => L("resolve-thenable-" + v));
new Promise((res) => res(Promise.resolve("N"))).then((v) => L("resolve-promise-" + v));

// Async function returning a promise takes extra ticks versus returning a value.
(async () => Promise.resolve("ret-p"))().then((v) => L("async-return-" + v));
(async () => "ret-v")().then((v) => L("async-return-" + v));

// Throwing thenable getter.
const bad = { get then() { L("bad-then-getter"); throw new Error("getter"); } };
Promise.resolve().then(() => bad).catch((e) => L("bad-thenable-" + e.message));

// A then callback that is not callable passes the value through.
Promise.resolve("pass").then(42).then((v) => L("noncallable-" + v));

// await inside try/catch/finally.
(async () => {
  try { await Promise.reject("in-try"); }
  catch (e) { L("await-catch-" + e); await null; L("catch-after-await"); }
  finally { L("await-finally"); }
})();

// Nested queueMicrotask runs after everything already queued.
queueMicrotask(() => { L("qm-2"); queueMicrotask(() => L("qm-2-nested")); });

// Promise.resolve on a promise returns it unchanged (no extra tick).
const same = Promise.resolve("same");
L("identity-" + (Promise.resolve(same) === same));

L("sync-2");

// Printed from frame(), which the driver calls only after the post-eval drain
// emptied the queue -- the same point the device's first frame runs.
globalThis.frame = () => { log.forEach((s, i) => print(i, s)); log.length = 0; };
