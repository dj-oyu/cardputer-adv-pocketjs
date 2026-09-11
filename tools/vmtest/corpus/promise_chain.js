// Long Promise chains: thousands of jobs in one drain. L1's job budget cuts
// such a drain into pieces; the printed order and values must not change.
let p = Promise.resolve(0);
for (let i = 1; i <= 3000; i++) p = p.then((v) => v + i);
p.then((v) => print("then-chain", v));

// Sequential awaits in a loop: each iteration suspends and resumes a frame.
(async () => {
  let s = 0;
  for (let i = 0; i < 2000; i++) s += await i;
  print("await-loop", s);
})();

// Rejection travelling down a chain past then() without handlers.
let r = Promise.reject(new Error("deep"));
for (let i = 0; i < 500; i++) r = r.then(() => "never");
r.catch((e) => print("reject-chain", e.message));

// Combinators.
const mk = (v, ok) => (ok ? Promise.resolve(v) : Promise.reject(v));
Promise.all([mk(1, true), 2, mk(3, true)]).then((v) => print("all", v.join()));
Promise.all([mk(1, true), mk("x", false), mk("y", false)]).catch((e) => print("all-reject", e));
Promise.allSettled([mk(1, true), mk("no", false)]).then((v) =>
  print("allSettled", v.map((x) => x.status + ":" + (x.value ?? x.reason)).join()));
Promise.race([new Promise(() => {}), mk("fast", true)]).then((v) => print("race", v));
Promise.any([mk("a", false), mk("b", true)]).then((v) => print("any", v));
Promise.any([mk("a", false), mk("b", false)]).catch((e) =>
  print("any-agg", e.constructor.name, e.errors.join()));

// A chain whose jobs keep enqueueing jobs: drain only ends when it stops.
let ticks = 0;
function tick() { if (++ticks < 1000) Promise.resolve().then(tick); else print("self-enqueue", ticks); }
tick();

// Many pending promises resolved together (fan-out on one resolve).
let release;
const gate = new Promise((res) => { release = res; });
let fan = 0;
for (let i = 0; i < 200; i++) gate.then((v) => { fan += v; });
gate.then(() => Promise.resolve()).then(() => print("fan-out", fan));
release(2);
print("sync-end");
