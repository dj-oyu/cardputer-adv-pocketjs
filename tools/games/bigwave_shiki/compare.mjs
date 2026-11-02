// BIG WAVE's move to Shiki, the compare half (capture.mjs): every captured
// draw runs the old program's rows in double, the @shiki function of the same
// name in double (an AST walk) and in Shiki's fixed point (shiki_fix.mjs).
// Reports, per program, the largest coordinate difference old vs @shiki in
// double (the translation) and fixed vs double (Q16), and the endpoints that
// land on another pixel. cubic is compared by its control points.
import fs from 'node:fs';
const K = new URL('../../kasane_ir/', import.meta.url).pathname;
const {findPlans, planAst} = await import(K + 'plan_js.mjs');
const {qdecls} = await import(K + 'shiki_q.mjs');
const {Fail, FIXM, emitFix} = await import(K + 'shiki_fix.mjs');
const {draws} = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const text = fs.readFileSync(process.argv[3], 'utf8'), qd = qdecls(text);
const plans = new Map(findPlans(text, 'prog', ['shiki']).map(p => [p.name, p]));
// ---- the old VM's rows in double: coordinates of move/line/plot, cubic's 8 control values
function runOld(rows, inp) {
  const r = new Float64Array(16), out = []; let pc = 0; const st = [];
  const end = new Map(), open = [];
  rows.forEach((x, i) => { if (x[0] === 5 || x[0] === 10) open.push(i); if (x[0] === 6) end.set(open.pop(), i); });
  let guard = 0;
  while (pc < rows.length && guard++ < 1e6) {
    const [o, d, a, b, imm, aux] = rows[pc]; let nx = pc + 1;
    switch (o) {
      case 0: r[d] = imm; break; case 1: r[d] = inp[a] ?? 0; break;
      case 2: r[d] = r[a] + r[b]; break; case 3: r[d] = r[a] * r[b]; break; case 4: r[d] = Math.sin(r[a]); break;
      case 5: case 10: { const n = o === 5 ? a : r[a]; if (!n) nx = end.get(pc) + 1; else st.push({pc, n}); break; }
      case 6: { const t = st[st.length - 1]; if (--t.n) nx = t.pc + 1; else st.pop(); break; }
      case 7: case 8: case 9: out.push(r[a], r[b]); break;
      case 11: if (r[a] > r[b]) { const t = st.pop(); nx = end.get(t.pc) + 1; } break;
      case 12: case 13: out.push(r[a], r[b]); break;
      case 14: for (let k = 0; k < 8; k++) out.push(r[k]); break;
      default: throw new Error('op ' + o);
    }
    pc = nx;
  }
  return out;
}
// ---- the @shiki functions in double (an AST walk) and in fixed (shiki_fix)
function runF64(ast, inputs, params, args) {
  const env = new Map(), out = [];
  const get = n => { if (!env.has(n)) throw new Error('unbound ' + n); return env.get(n); };
  const ev = e => { switch (e.k) {
    case 'num': return +e.lit; case 'name': return get(e.v); case 'neg': return -ev(e.a);
    case '+': return ev(e.a) + ev(e.b); case '-': return ev(e.a) - ev(e.b); case '*': return ev(e.a) * ev(e.b);
    case '/': return ev(e.a) / +e.b.lit; case 'call': return Math.sin(ev(e.args[0]));
  } throw new Error(e.k); };
  const run = list => { for (const s of list) {
    if (s.k === 'input') s.d.forEach(d => env.set(d.n, inputs[d.k] ?? 0));
    else if (s.k === 'param') env.set(s.n, args[s.index]);
    else if (s.k === 'decl') s.d.forEach(d => env.set(d.n, ev(d.e)));
    else if (s.k === 'assign') { const v = ev(s.e), o = get(s.n); env.set(s.n, s.o === '=' ? v : s.o === '+=' ? o + v : s.o === '-=' ? o - v : o * v); }
    else if (s.k === 'repeat') { const n = ev(s.n); for (let i = 0; i < n; i++) if (run(s.body) === 'brk') break; }
    else if (s.k === 'break') { if (ev(s.a) > ev(s.b)) return 'brk'; }
    else if (s.k === 'draw') { const a = s.args.map(ev);
      if (s.f === 'cubic') out.push(...a.slice(2)); else out.push(a[0], a[1]); }
    else throw new Error(s.k);
  } };
  run(ast);
  return out;
}
function runFix(fn, inputs, args) {
  const out = [];
  const api = {move: (x, y) => out.push(x, y), line: (x, y) => out.push(x, y), plot: (x, y) => out.push(x, y),
    cubic: (n, c, ...p) => out.push(...p)};
  try { fn(FIXM, api, Fail, inputs, args); } catch (e) { if (!(e instanceof Fail)) throw e; out.push('FAIL ' + e.status); }
  return out;
}
const argsOf = (k, a) => ({cu: [a.c, a.n], sh: [a.c, a.n], se: [a.t, a.s, a.g], sk: [a.k, a.t], fo: [a.l, a.n, a.d, a.c]}[k] ?? []);
// big_wave.js's args() (from the capture's spec: key, set, tier), copied here.
const SETS = [{cu: 0xafff, sh: 0x455f, se: 0x2b5a, sk: 0xfd8a, fo: 0xffff}, {cu: 0x07ff, sh: 0x03df, se: 0x0272, sk: 0xffff, fo: 0xffff},
  {cu: 0x5fff, sh: 0xb01f, se: 0x600c, sk: 0xfd20, fo: 0xffdf}], HS = [5, 6.5, 8], TIER = [[44, 5, 4], [54, 6, 5], [64, 7, 6]];
function A(k, set, tier) {
  const P = SETS[set], H = HS[set], tr = TIER[tier], a = {n: tr[1]};
  if (k === 'cu') a.c = P.cu; if (k === 'sh') a.c = P.sh;
  if (k === 'se') { a.t = P.cu; a.s = P.se; a.g = P.fo; }
  if (k === 'sk') { a.k = P.sk; a.t = P.se; }
  if (k === 'fo') { a.l = .25 * H; a.n = tr[2]; a.d = -1.55 * H / (tr[2] - 1); a.c = P.fo; }
  return a;
}
const rows = new Map();
const fixFn = new Map();
for (const d of draws) {
  const k = d.spec[0]; if (k.startsWith('sp')) continue;
  const p = plans.get(k); if (!p) throw new Error('no plan ' + k);
  if (!fixFn.has(k)) fixFn.set(k, emitFix(planAst(p), p.params, qd.get(k) ?? {}, p.data));
  const args = ['cu','sh','se','sk','fo'].includes(k) ? argsOf(k, A(k, d.spec[1], d.spec[2])) : [];
  const old = runOld(d.rows, d.inputs), f64 = runF64(planAst(p), d.inputs, p.params, args), fx = runFix(fixFn.get(k), d.inputs, args);
  const row = rows.get(k) ?? {n: 0, trans: 0, fix: 0, fixpx: 0, shape: 0, failfix: 0, coords: 0};
  rows.set(k, row); row.n++;
  if (old.length !== f64.length) { row.shape++; if (row.shape < 3) console.log('SHAPE', k, old.length, f64.length); continue; }
  if (fx.some(v => typeof v === 'string')) { row.failfix++; continue; }
  for (let i = 0; i < old.length; i++) {
    row.coords++;
    row.trans = Math.max(row.trans, Math.abs(old[i] - f64[i]));
    const e = Math.abs(fx[i] - f64[i]); row.fix = Math.max(row.fix, e);
    if (Math.floor(fx[i] + .5) !== Math.round(f64[i])) row.fixpx++;
  }
}
console.log('plan  draws   coords | translation max (old rows vs @shiki, double) | fixed max px, endpoints on another pixel | shape diff, fixed failed');
for (const [k, r] of rows) console.log(k.padEnd(4), String(r.n).padStart(6), String(r.coords).padStart(8), '|', r.trans.toExponential(2).padStart(10), '|',
  r.fix.toFixed(4).padStart(8), String(r.fixpx).padStart(7), '|', r.shape, r.failfix);
