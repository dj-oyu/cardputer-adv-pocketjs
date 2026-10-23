// TDZ semantics around the compile-time check elision (tdz_elide_checks in
// quickjs.c, docs/vm/tdz-elision.md). The "throws" half must still raise a
// ReferenceError; the "elided" half must keep its values. Expected output
// is node's. tools/vmtest/tdz_elision.c checks which of these lose their
// checks.
function run(name, f) {
  try { print(name, String(f())); } catch (e) { print(name, e.constructor.name); }
}

// ---- still throws
function readBefore() { x; let x = 1; return x; }
function selfInit() { let x = x + 1; return x; }
function closureBefore() { const f = () => x; f(); let x = 1; return x; }
function hoistedBefore() { g(); let x = 1; function g() { return x; } return x; }
function switchCase(k) { switch (k) { case 0: let x = 1; return x; case 1: return x; } }
function loopStale() {
  const out = [];
  for (let j = 0; j < 2; j++) { if (j === 1) out.push(y); let y = j; out.push(y); }
  return out.join();
}
function loopStaleBlock() {
  const out = [];
  for (let j = 0; j < 3; j++) {
    { if (j > 0) { try { out.push(z); } catch (e) { out.push('tdz'); } } const z = j; out.push(z); }
  }
  return out.join();
}
function labeledBreak(n) { { l: { if (n) break l; return 0; } return v; let v = 2; } }
function catchBefore(t) { { try { if (t) throw 0; } catch (e) { return k; } let k = 1; return k; } }
function finallyBefore(t) { { try { if (t) return 'early'; } finally { typeof m; } let m = 1; return m; } }
function typeofTdz() { return typeof q; let q; }
function* genTdz() { yield 1; yield x; let x = 2; }
function classBefore() { new C(); class C {} return 'ok'; }
function destructDefault() { let { a = a } = {}; return a; }
function assignBefore() { x = 1; let x; return x; }
function deadPathRead(c) { { if (c) { return w; } let w = 3; return w; } }
function continueSkipsInit(n) {
  let s = '';
  for (let i = 0; i < n; i++) { { if (i === 1) { s += typeof h === 'number' ? 'n' : 'u'; } if (i === 0) continue; const h = i; s += h; } }
  return s;
}
function classHeritage() { class X extends X {} return 'ok'; }
function classKey() { class Y { [Y]() {} } return 'ok'; }
function withTdz(o) { with (o) { return q; } let q; }
class B {}
class D extends B { constructor() { this.x = 1; super(); } }

run('readBefore', readBefore);
run('selfInit', selfInit);
run('closureBefore', closureBefore);
run('hoistedBefore', hoistedBefore);
run('switchCase0', () => switchCase(0));
run('switchCase1', () => switchCase(1));
run('loopStale', loopStale);
run('loopStaleBlock', loopStaleBlock);
run('labeledBreak0', () => labeledBreak(0));
run('labeledBreak1', () => labeledBreak(1));
run('catchBefore0', () => catchBefore(0));
run('catchBefore1', () => catchBefore(1));
run('finallyBefore0', () => finallyBefore(0));
run('finallyBefore1', () => finallyBefore(1));
run('typeofTdz', typeofTdz);
run('genTdz', () => { const g = genTdz(); g.next(); return g.next().value; });
run('classBefore', classBefore);
run('destructDefault', destructDefault);
run('assignBefore', assignBefore);
run('deadPathRead0', () => deadPathRead(0));
run('deadPathRead1', () => deadPathRead(1));
run('continueSkipsInit', () => continueSkipsInit(3));
run('classHeritage', classHeritage);
run('classKey', classKey);
run('withTdz0', () => withTdz({}));
run('withTdz1', () => withTdz({ q: 5 }));
run('derivedThis', () => new D());

// ---- elided, values unchanged
function straight(a) { const x = a + 1, y = x * 2; return x + y; }
function joinBranches(c) { let r; if (c) { const t = 1; r = t; } else { const t = 2; r = t + 1; } return r; }
function loopBody(n) { let s = 0; for (let i = 0; i < n; i++) { const sq = i * i; s += sq; } return s; }
function whileBreak(a) {
  let s = 0, i = 0;
  while (true) { const v = a[i++]; if (v === undefined) break; if (v < 0) continue; s += v; }
  return s;
}
function forOf(xs) { let s = 0; for (const x of xs) s += x; return s; }
function capturedLocal() { let n = 0; const inc = () => ++n; inc(); inc(); return n; }
function freshPerIteration(n) { const out = []; for (let i = 0; i < n; i++) { const o = { i }; out.push(o.i); } return out.join(); }
function catchRead() { let a = 1; try { a = 2; throw 0; } catch (e) { return a; } }
function labeledContinue() {
  let c = 0;
  outer: for (let i = 0; i < 3; i++) { for (let j = 0; j < 3; j++) { if (j === i) continue outer; c += j; } }
  return c;
}
function closuresPerIteration() {
  const fs = [];
  for (let i = 0; i < 3; i++) { const k = i * 10; fs.push(() => k + i); }
  return fs.map(f => f()).join();
}
function counters(n) { let a = 0; for (let i = 0; i < n; i++) { a++; a += 2; } return a; }
function finallyAfter() { let r = 'a'; try { r += 'b'; } finally { r += 'c'; } const t = r + 'd'; return t; }
function withAfter(o) { let a = 1; with (o) { a = a + 1; } return a; }
function switchInLoop(n) {
  let s = '';
  for (let i = 0; i < n; i++) { switch (i % 3) { case 0: { const c = 'a'; s += c; break; } case 1: s += i; break; default: { const d = i * 2; s += d; } } }
  return s;
}

run('straight', () => straight(3));
run('joinBranches1', () => joinBranches(1));
run('joinBranches0', () => joinBranches(0));
run('loopBody', () => loopBody(5));
run('whileBreak', () => whileBreak([1, -2, 3, 4]));
run('forOf', () => forOf([1, 2, 3]));
run('capturedLocal', capturedLocal);
run('freshPerIteration', () => freshPerIteration(4));
run('catchRead', catchRead);
run('labeledContinue', labeledContinue);
run('closuresPerIteration', closuresPerIteration);
run('counters', () => counters(4));
run('finallyAfter', finallyAfter);
run('withAfter0', () => withAfter({}));
run('withAfter1', () => withAfter({ a: 10 }));
run('switchInLoop', () => switchInLoop(6));
