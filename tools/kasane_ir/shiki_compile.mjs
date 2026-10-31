// The @shiki compiler: a plan written as a JS function (plan_js.mjs's subset,
// docs/kasane/js-to-ir.md sec.5) to Shiki v1 bytes (docs/kasane/shiki-spec.md
// sec.3), with the Q formats of sec.8.3. Host only (Node).
//
//   import {compileShiki} from './shiki_compile.mjs';
//   const {bytes, count, inputsQ, argsQ, warnings} = compileShiki(plan, qdecl);
//
// Stage 1 of sec.11: the 32-bit instructions, SIN, loops, breaks and
// MOVE/PLOT/LINE. cubic(), linePattern() and data regions throw (their
// engine instructions are not there yet).
//
// The passes follow kir.mjs (the float compiler): lower to virtual registers
// with every value computed at the Q its consumer asks for (sec.8.3 rule 2,
// the same rules shiki_fix.mjs evaluates, from shiki_q.mjs); hoist constants,
// inputs and arguments to the top-level statement of their first use; drop
// dead definitions; colour the interference graph into r0..r15; when that
// fails, re-materialise a hoisted value at each use and try again.
//
// What the result must equal is shiki_fix.mjs's evaluation of the same AST,
// bit for bit (test_shiki_compile.mjs). Coordinates go to the engine at Q16
// as they are: the engine rounds them (sec.4.5, v1.1).
import {planAst} from './plan_js.mjs';
import {litQ, qRules, TURN} from './shiki_q.mjs';

export const OP = {SET: 0, INPUT: 1, ADD: 2, MUL: 3, SIN: 4, REPEAT: 5, END: 6, MOVE: 7, PLOT: 8, LINE: 9,
  REPEAT_REG: 10, BREAK_IF_GT: 11, PLOT_C: 12, LINE_C: 13, CUBIC: 14, LINE_PATTERN: 15, SUB: 16, NEG: 17,
  SHIFT: 18, ADD_H: 19, SUB_H: 20, MUL_H: 21, SHIFT_H: 22, EACH: 23, FETCH: 24, PUT: 25, LOOKUP: 26};
const REGS = 16, CODE = 64;

// Raw value of a literal at Q q (sec.2.4's rounding), or a compile error
// (rule 9) when it does not fit.
function raw(v, q) {
  const x = v * 2 ** q;
  if (!Number.isFinite(x) || Math.abs(Math.round(x)) > 2 ** 31 - 1) throw new Error(`literal ${v} does not fit Q${q}`);
  return Math.sign(x) * Math.floor(Math.abs(x) + .5) | 0;
}
const S = s => { if (s > 62) throw new Error(`MUL shift ${s} over 62`); if (s < -31) throw new Error(`MUL shift ${s} under -31`); return s; };
const K = k => { if (Math.abs(k) > 31) throw new Error(`SHIFT ${k} outside -31..31`); return k; };

// Operands an instruction reads and whether it writes dst.
const READS = {
  [OP.ADD]: ['a', 'b'], [OP.SUB]: ['a', 'b'], [OP.MUL]: ['a', 'b'], [OP.NEG]: ['a'], [OP.SHIFT]: ['a'], [OP.SIN]: ['a'],
  [OP.REPEAT_REG]: ['a'], [OP.BREAK_IF_GT]: ['a', 'b'], [OP.MOVE]: ['a', 'b'], [OP.PLOT]: ['a', 'b'], [OP.LINE]: ['a', 'b'],
  [OP.PLOT_C]: ['a', 'b', 'dst'], [OP.LINE_C]: ['a', 'b', 'dst'],
};
const WRITES = new Set([OP.SET, OP.INPUT, OP.ADD, OP.SUB, OP.MUL, OP.NEG, OP.SHIFT, OP.SIN]);
const uses = i => (READS[i.op] ?? []).map(f => i[f]);
const defines = i => WRITES.has(i.op);

// ------------------------------------------------------------- lowering
function lower(ast, qdecl) {
  const {Q, natural} = qRules(ast, qdecl);
  const code = [], vregs = [], names = new Map(), pools = new Map(), warnings = [];
  const inputsQ = [], argsQ = [];
  const assigned = new Set();
  (function scan(s) { for (const x of s) { if (x.k === 'assign') assigned.add(x.n); if (x.body) scan(x.body); } })(ast);
  const vreg = (kind, name) => vregs.push({kind, name}) - 1;
  const emit = i => (code.push(i), i);
  // A hoisted value (constant, input, argument), defined once.
  function pool(key, def) {
    if (!pools.has(key)) { const v = vreg('pool', key); vregs[v].def = def; pools.set(key, v); }
    return pools.get(key);
  }
  const cst = r => pool('c' + r, {op: OP.SET, imm: r});
  // Descriptor -> virtual register.
  const mat = x => x.t === 'r' ? x.vr : x.t === 'c' ? cst(x.raw)
    : x.t === 'i' ? pool('i' + x.k, {op: OP.INPUT, a: x.k}) : pool('p' + x.n, {op: OP.SET, imm: 0, patch: {arg: x.n, field: 0}});
  // Common subexpressions: a pure instruction on the same operands (at the
  // same version of a variable) is computed once. Forgotten at every loop
  // boundary, and a variable's write moves its version on.
  const version = new Map();
  let cse = new Map();
  const ver = v => version.get(v) ?? 0;
  const wrote = v => version.set(v, ver(v) + 1);
  const COMMUTES = new Set([OP.ADD, OP.MUL]);
  function op(o, f) {
    let ops = (READS[o] ?? []).map(k => f[k] + '@' + ver(f[k]));
    if (COMMUTES.has(o)) ops = ops.sort();
    const key = [o, ...ops, f.imm ?? '', f.aux ?? ''].join('|');
    if (cse.has(key)) return {t: 'r', vr: cse.get(key)};
    const dst = vreg('temp');
    cse.set(key, dst);
    return {t: 'r', vr: dst, inst: emit({op: o, dst, ...f}), key};
  }
  function read(n) {
    const b = names.get(n);
    if (!b) throw new Error(`${n} is not defined`);
    return b.kind === 'alias' ? b.x : {t: 'r', vr: b.vr, var: true};
  }
  // A factor of * (rule 4): [descriptor, its Q].
  function factor(e) {
    if (e.k === 'neg' && e.a.k === 'num') e = {k: 'num', lit: String(-e.a.lit)};
    if (e.k === 'name') return [read(e.v), Q(e.v)];
    if (e.k === 'num') { const q = litQ(+e.lit); return [{t: 'c', raw: raw(+e.lit, q)}, q]; }
    return [ev(e, 16), 16];
  }
  function mul(a, b, s) {
    if (s >= 0) return op(OP.MUL, {a: mat(a), b: mat(b), aux: S(s)});
    S(s);
    const p = op(OP.MUL, {a: mat(a), b: mat(b), aux: 0});
    return op(OP.SHIFT, {a: p.vr, imm: K(-s)});
  }
  // e at Q q (rule 2).
  function ev(e, q) {
    switch (e.k) {
      case 'num': return {t: 'c', raw: raw(+e.lit, q)};
      case 'name': {
        const x = read(e.v), d = K(q - Q(e.v));
        return d ? op(OP.SHIFT, {a: mat(x), imm: d}) : x;
      }
      case 'neg': return e.a.k === 'num' ? ev({k: 'num', lit: String(-e.a.lit)}, q) : op(OP.NEG, {a: mat(ev(e.a, q))});
      case '+': return op(OP.ADD, {a: mat(ev(e.a, q)), b: mat(ev(e.b, q))});
      case '-': return op(OP.SUB, {a: mat(ev(e.a, q)), b: mat(ev(e.b, q))});
      case '*': {
        // A factor of 1 is a shift to q, and of -1 at q a NEG: the same bits
        // as the MUL (the literal is exact at Q30, floor(x 2^30 / 2^s)).
        const one = f => f.k === 'num' ? +f.lit : f.k === 'neg' && f.a.k === 'num' ? -f.a.lit : null;
        for (const [u, w] of [[e.a, e.b], [e.b, e.a]]) {
          const c = one(u);
          if (c !== 1 && c !== -1) continue;
          const [x, qx] = factor(w);
          if (c === 1) return qx === q ? x : op(OP.SHIFT, {a: mat(x), imm: K(q - qx)});
          if (qx === q) return op(OP.NEG, {a: mat(x)});
        }
        const [a, qa] = factor(e.a), [b, qb] = factor(e.b);
        return mul(a, b, qa + qb - q);
      }
      case '/': {
        const c = +e.b.lit, k = Math.log2(Math.abs(c));
        if (Number.isInteger(k) && k >= 0) {
          let r;
          if (e.a.k === '*') { const [a, qa] = factor(e.a.a), [b, qb] = factor(e.a.b); r = mul(a, b, qa + qb - q + k); }
          else r = k ? op(OP.SHIFT, {a: mat(ev(e.a, q)), imm: K(-k)}) : ev(e.a, q);
          return c < 0 ? op(OP.NEG, {a: mat(r)}) : r;
        }
        return ev({k: '*', a: e.a, b: {k: 'num', lit: String(1 / c)}}, q);
      }
      case 'call': {
        const turns = op(OP.MUL, {a: mat(ev(e.args[0], 16)), b: cst(TURN), aux: 32});
        const s = op(OP.SIN, {a: turns.vr});
        return q === 16 ? s : op(OP.SHIFT, {a: s.vr, imm: K(q - 16)});
      }
    }
    throw new Error(`bad expression ${e.k}`);
  }
  // e at Q q into the variable register vr: the last instruction that made a
  // fresh temporary writes vr itself; anything else is copied (SHIFT by 0).
  function into(e, q, vr) {
    const x = ev(e, q);
    // The temporary is renamed to the variable: its CSE entry would now name
    // a register that the variable's next write changes.
    if (x.inst && !x.var) { x.inst.dst = vr; cse.delete(x.key); wrote(vr); return; }
    if (x.t === 'r' && x.vr === vr) return;
    wrote(vr);
    if (x.t === 'c') emit({op: OP.SET, dst: vr, imm: x.raw});
    else if (x.t === 'i') emit({op: OP.INPUT, dst: vr, a: x.k});
    else if (x.t === 'p') emit({op: OP.SET, dst: vr, imm: 0, patch: {arg: x.n, field: 0}});
    else emit({op: OP.SHIFT, dst: vr, a: x.vr, imm: 0, copy: x.vr});
  }
  // A coordinate: at Q16; the engine rounds it (sec.4.5, v1.1).
  const coord = e => mat(ev(e, 16));
  // An integer argument (colour, count): an immediate, a patched argument or
  // a register.
  function int(e, what, lo, hi) {
    const x = ev(e, 0);
    if (x.t === 'c') { if (x.raw < lo || x.raw > hi) throw new Error(`${what} ${x.raw} outside ${lo}..${hi}`); return {imm: x.raw}; }
    if (x.t === 'p') return {patch: x.n};
    return {reg: mat(x)};
  }
  function stmts(list) {
    for (const s of list) {
      switch (s.k) {
        case 'input': for (const d of s.d) { names.set(d.n, {kind: 'alias', x: {t: 'i', k: d.k}}); inputsQ[d.k] = Q(d.n); } break;
        case 'param': names.set(s.n, {kind: 'alias', x: {t: 'p', n: s.index}}); argsQ[s.index] = Q(s.n); break;
        case 'decl':
          for (const d of s.d) {
            if (assigned.has(d.n)) {
              if (s.kind === 'const') throw new Error(`const ${d.n} is assigned`);
              const b = {kind: 'var', vr: vreg('var', d.n)};
              into(d.e, Q(d.n), b.vr); names.set(d.n, b);
            } else {
              let x = ev(d.e, Q(d.n));
              // A snapshot of a variable must not follow its later writes.
              if (x.var) { const v = vreg('var', d.n); emit({op: OP.SHIFT, dst: v, a: x.vr, imm: 0, copy: x.vr}); x = {t: 'r', vr: v}; }
              names.set(d.n, {kind: 'alias', x: {...x, inst: undefined}});
            }
          }
          break;
        case 'assign': {
          const b = names.get(s.n);
          if (!b || b.kind !== 'var') throw new Error(`${s.n} is not a variable`);
          into(s.o === '=' ? s.e : {k: s.o[0], a: {k: 'name', v: s.n}, b: s.e}, Q(s.n), b.vr);
          break;
        }
        case 'repeat': {
          const n = ev(s.n, 0);
          if (n.t === 'c') {
            if (n.raw < 1 || n.raw > 255) throw new Error(`REPEAT ${n.raw} outside 1..255`);
            emit({op: OP.REPEAT, imm: n.raw});
          } else if (n.t === 'p') emit({op: OP.REPEAT, imm: 0, patch: {arg: n.n, field: 0}});
          else emit({op: OP.REPEAT_REG, a: mat(n)});
          cse = new Map();
          stmts(s.body);
          emit({op: OP.END});
          cse = new Map();
          break;
        }
        case 'break': {
          const qa = natural(s.a), qb = natural(s.b), q = Math.max(qa ?? qb ?? 16, qb ?? qa ?? 16);
          emit({op: OP.BREAK_IF_GT, a: mat(ev(s.a, q)), b: mat(ev(s.b, q))});
          break;
        }
        case 'draw': {
          const f = s.f, A = s.args;
          if (f === 'cubic' || f === 'linePattern') throw new Error(`${f}() is not in stage 1 (shiki-spec.md sec.4.6, 4.7)`);
          const o = {move: OP.MOVE, plot: OP.PLOT, line: OP.LINE}[f];
          if (o === undefined) throw new Error(`unknown statement ${f}`);
          const x = coord(A[0]), y = coord(A[1]);
          if (o === OP.MOVE) { emit({op: o, a: x, b: y}); break; }
          const c = int(A[2], 'colour', 0, 65535);
          if (c.reg !== undefined) emit({op: o === OP.PLOT ? OP.PLOT_C : OP.LINE_C, a: x, b: y, dst: c.reg});
          else if (c.patch !== undefined) emit({op: o, a: x, b: y, aux: 0, patch: {arg: c.patch, field: 1}});
          else emit({op: o, a: x, b: y, aux: c.imm});
          break;
        }
      }
    }
  }
  stmts(ast);
  return {code, vregs, inputsQ, argsQ, warnings};
}

// ---------------------------------------------------- control flow, liveness
function structure(code) {
  const end = new Map(), stack = [];
  code.forEach((i, pc) => {
    if (i.op === OP.REPEAT || i.op === OP.REPEAT_REG) stack.push(pc);
    if (i.op === OP.END) end.set(stack.pop(), pc);
  });
  return end;
}
function successors(code) {
  const end = structure(code), open = [], out = [];
  code.forEach((i, pc) => {
    const s = [pc + 1];
    if (i.op === OP.REPEAT || i.op === OP.REPEAT_REG) { open.push(pc); s.push(end.get(pc) + 1); }
    if (i.op === OP.END) s.push(open.pop() + 1);
    if (i.op === OP.BREAK_IF_GT) s.push(end.get(open[open.length - 1]) + 1);
    out.push(s.filter(t => t <= code.length));
  });
  return out;
}
function liveness(code) {
  const succ = successors(code), liveIn = code.map(() => new Set()), liveOut = code.map(() => new Set());
  for (let changed = true; changed;) {
    changed = false;
    for (let pc = code.length - 1; pc >= 0; pc--) {
      const out = new Set();
      for (const t of succ[pc]) if (t < code.length) for (const v of liveIn[t]) out.add(v);
      const inn = new Set(out);
      if (defines(code[pc])) inn.delete(code[pc].dst);
      for (const u of uses(code[pc])) inn.add(u);
      if (inn.size !== liveIn[pc].size || out.size !== liveOut[pc].size) changed = true;
      liveIn[pc] = inn; liveOut[pc] = out;
    }
  }
  return {liveIn, liveOut};
}
function removeDead(code) {
  for (;;) {
    const {liveOut} = liveness(code);
    const keep = code.filter((i, pc) => !defines(i) || liveOut[pc].has(i.dst));
    if (keep.length === code.length) return code;
    code = keep;
  }
}
// Hoisted values go before the top-level statement of their first use.
function placePools(code, vregs) {
  const first = new Map();
  let depth = 0, top = 0;
  code.forEach((i, pc) => {
    if (depth === 0) top = pc;
    for (const u of uses(i)) if (vregs[u].kind === 'pool' && !first.has(u)) first.set(u, top);
    if (i.op === OP.REPEAT || i.op === OP.REPEAT_REG) depth++;
    if (i.op === OP.END) depth--;
  });
  for (const [vr, pc] of [...first].sort((a, b) => b[1] - a[1] || b[0] - a[0]))
    code.splice(pc, 0, {...vregs[vr].def, dst: vr});
  return code;
}
// A hoisted value defined again right before each use.
function remat(code, vregs, vr) {
  const out = [];
  for (const i of code) {
    if (defines(i) && i.dst === vr) continue;
    if (uses(i).includes(vr)) {
      const t = vregs.push({kind: 'remat', name: vregs[vr].name}) - 1;
      out.push({...vregs[vr].def, dst: t});
      const j = {...i};
      for (const f of READS[i.op] ?? []) if (j[f] === vr) j[f] = t;
      out.push(j);
    } else out.push(i);
  }
  vregs[vr].gone = true;
  return out;
}
function color(code) {
  const {liveOut} = liveness(code), adj = new Map(), nodes = new Set();
  const edge = (a, b) => {
    if (a === b) return;
    (adj.get(a) ?? adj.set(a, new Set()).get(a)).add(b);
    (adj.get(b) ?? adj.set(b, new Set()).get(b)).add(a);
  };
  code.forEach((i, pc) => {
    for (const u of uses(i)) nodes.add(u);
    if (defines(i)) { nodes.add(i.dst); for (const v of liveOut[pc]) if (v !== i.copy) edge(i.dst, v); }
  });
  const rest = new Set(nodes), stack = [];
  const deg = v => [...(adj.get(v) ?? [])].filter(w => rest.has(w)).length;
  while (rest.size) {
    let pick = null;
    for (const v of rest) if (deg(v) < REGS) { pick = v; break; }
    if (pick === null) pick = [...rest].sort((a, b) => deg(b) - deg(a))[0];
    rest.delete(pick); stack.push(pick);
  }
  const copyOf = new Map();
  code.forEach(i => { if (i.copy !== undefined) { copyOf.set(i.dst, i.copy); copyOf.set(i.copy, i.dst); } });
  const reg = new Map();
  while (stack.length) {
    const v = stack.pop(), taken = new Set([...(adj.get(v) ?? [])].map(w => reg.get(w)));
    const want = reg.get(copyOf.get(v));
    let r = want !== undefined && !taken.has(want) ? want : -1;
    for (let k = 0; r < 0 && k < REGS; k++) if (!taken.has(k)) r = k;
    if (r < 0) return null;
    reg.set(v, r);
  }
  return reg;
}

// ------------------------------------------------------------------ bytes
function encode(code, inputsQ, argsQ) {
  const patches = [];
  code.forEach((i, pc) => { if (i.patch) patches.push([pc, i.patch.arg, i.patch.field]); });
  patches.sort((x, y) => x[0] - y[0] || x[2] - y[2]);
  const head = 12 + inputsQ.length + argsQ.length + 4 * patches.length, body = (head + 3) & ~3;
  const b = new Uint8Array(body + 12 * code.length), dv = new DataView(b.buffer);
  b.set([0x53, 0x4b, 0x49, 0x31, 1, 0, code.length, inputsQ.length, argsQ.length, 0, patches.length, 0]);
  b.set(inputsQ, 12); b.set(argsQ, 12 + inputsQ.length);
  patches.forEach((p, k) => b.set([...p, 0], 12 + inputsQ.length + argsQ.length + 4 * k));
  code.forEach((i, k) => {
    const o = body + 12 * k;
    b.set([i.op, i.dst ?? 0, i.a ?? 0, i.b ?? 0], o);
    dv.setInt32(o + 4, i.imm ?? 0, true);
    dv.setUint16(o + 8, i.aux ?? 0, true);
  });
  return b;
}

// plan: plan_js.mjs's findPlans() entry; qdecl: {name: Q} from its @q.
export function compileShiki(plan, qdecl = {}) {
  const ast = planAst(plan);
  const {code: lowered, vregs, inputsQ, argsQ, warnings} = lower(ast, qdecl);
  for (let k = 0; k < plan.inputs.length; k++) inputsQ[k] ??= 16;
  let code = removeDead(placePools(lowered, vregs));
  const rematerialised = [];
  let reg;
  while (!(reg = color(code))) {
    // Re-materialise the hoisted value with the fewest uses.
    const counts = new Map();
    code.forEach(i => { for (const u of uses(i)) if (vregs[u].kind === 'pool') counts.set(u, (counts.get(u) ?? 0) + 1); });
    const pick = [...counts].sort((a, b) => a[1] - b[1])[0];
    if (!pick) throw new Error(`more than ${REGS} values live at once`);
    rematerialised.push(vregs[pick[0]].name);
    code = removeDead(remat(code, vregs, pick[0]));
  }
  const R = v => reg.get(v);
  let out = code.map(i => {
    const j = {...i};
    if (defines(i)) j.dst = R(i.dst);
    for (const f of READS[i.op] ?? []) j[f] = R(i[f]);
    return j;
  }).filter(j => !(j.copy !== undefined && j.op === OP.SHIFT && j.imm === 0 && j.a === j.dst));
  // Registers start at 0 each draw: a top-level SET 0 before any write to its
  // register changes nothing.
  const written = new Set();
  let depth = 0;
  out = out.filter(j => {
    const drop = j.op === OP.SET && j.imm === 0 && !j.patch && depth === 0 && !written.has(j.dst);
    if (defines(j)) written.add(j.dst);
    if (j.op === OP.REPEAT || j.op === OP.REPEAT_REG) depth++;
    if (j.op === OP.END) depth--;
    return !drop;
  });
  if (!out.length) out.push({op: OP.SET, dst: 0, imm: 0});
  if (out.length > CODE && !process.env.SHIKI_NO_LIMIT) throw new Error(`${out.length} instructions, over ${CODE}`);
  const qIn = inputsQ.slice(0, plan.inputs.length), qArg = plan.params.map((_, k) => argsQ[k] ?? 16);
  return {bytes: encode(out, qIn, qArg), code: out, count: out.length, inputsQ: qIn, argsQ: qArg, rematerialised, warnings};
}
