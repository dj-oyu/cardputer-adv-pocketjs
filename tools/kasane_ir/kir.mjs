// A compiler from a small JS-like language to ksn_proc_op IR (the float VM of
// main/ui/kasane/ksn_procedural.c), its one-letter text form (prog() in
// apps/derby/derby_prog.js) and a decompiler back. Host only (Node).
// docs/kasane/js-to-ir.md has the language, the passes and the measurements.
//
// Every transformation keeps the VM's float32 results bit for bit: operands
// are never reassociated, constants are folded only through + and * (exact in
// double then rounded once, as the VM's single-precision ops are), sin is
// never folded, x*1 is the only algebraic identity used. Dead code is removed
// by liveness, which also drops a dead ADD/MUL that could have gone non-finite
// (the VM would have failed the draw): the language's meaning is its visible
// output, not the hand IR's failure points.

export const OPS = 'SIAMNREVPLQBplC';
const FLD = ['14', '12', '123', '123', '12', '2', '', '23', '235', '235', '2', '23', '123', '123', '25'];
const FIELD = ['op', 'dst', 'a', 'b', 'value', 'color'];
export const O = {SET: 0, INPUT: 1, ADD: 2, MUL: 3, SIN: 4, REPEAT: 5, END: 6, MOVE: 7, PLOT: 8, LINE: 9,
  REPEAT_REG: 10, BREAK: 11, PLOTC: 12, LINEC: 13, CUBIC: 14};
export const REGS = 16, CODE = 64;
const f32 = Math.fround;

// ---------------------------------------------------------------- text form
export function parseText(src) {
  return src.trim().split(/\s+/).map(t => {
    const op = OPS.indexOf(t[0]);
    if (op < 0) throw new SyntaxError(`unknown op ${t}`);
    const f = FLD[op], v = t.length > 1 ? t.slice(1).split(',') : [];
    if (v.length !== f.length) throw new SyntaxError(`bad fields ${t}`);
    const i = {op, dst: 0, a: 0, b: 0, value: 0, color: 0, lit: null, par: {}};
    for (let j = 0; j < f.length; ++j) {
      const field = FIELD[+f[j]], s = v[j];
      if (s[0] === '$') i.par[field] = +s.slice(1);
      else { i[field] = +s; if (field === 'value') i.lit = s; }
    }
    return i;
  });
}
export function formatText(code) {
  return code.map(i => OPS[i.op] + [...FLD[i.op]].map(k => {
    const field = FIELD[+k];
    if (field in i.par) return '$' + i.par[field];
    if (field === 'value') return i.lit ?? shortest(i.value);
    return String(i[field]);
  }).join(',')).join(' ');
}
// prog()'s rows: [op, dst, a, b, value, color], parameters substituted.
export function assemble(code, args = []) {
  return code.map(i => {
    const r = [i.op, i.dst, i.a, i.b, i.value, i.color];
    for (const [field, n] of Object.entries(i.par)) r[FIELD.indexOf(field)] = args[n];
    if (i.lit != null && !('value' in i.par)) r[4] = +i.lit;
    return r;
  });
}
// The shortest decimal that registers as the same float32.
export function shortest(v) {
  if (Object.is(v, -0)) return '-0';
  for (let p = 1; p <= 9; ++p) {
    const s = String(+v.toPrecision(p));
    if (f32(+s) === v) return s.replace(/^0\./, '.').replace(/^-0\./, '-.');
  }
  return String(v);
}

// ---------------------------------------------------------- structure, CFG
function uses(i) {
  switch (i.op) {
    case O.ADD: case O.MUL: return [i.a, i.b];
    case O.SIN: case O.REPEAT_REG: return [i.a];
    case O.MOVE: case O.PLOT: case O.LINE: case O.BREAK: return [i.a, i.b];
    case O.PLOTC: case O.LINEC: return [i.dst, i.a, i.b];
    case O.CUBIC: return i.args ?? [0, 1, 2, 3, 4, 5, 6, 7];
    default: return [];
  }
}
const defines = i => i.op <= O.SIN;
function loops(code) {
  const end = new Map(), start = new Map(), stack = [];
  code.forEach((i, pc) => {
    if (i.op === O.REPEAT || i.op === O.REPEAT_REG) stack.push(pc);
    else if (i.op === O.END) { const s = stack.pop(); if (s === undefined) throw new Error(`END without loop at ${pc}`); end.set(s, pc); start.set(pc, s); }
  });
  if (stack.length) throw new Error('unclosed loop');
  return {end, start};
}
function successors(code) {
  const {end, start} = loops(code), inner = [], out = [];
  code.forEach((i, pc) => {
    const s = [];
    if (i.op === O.END) { s.push(start.get(pc) + 1); if (pc + 1 < code.length) s.push(pc + 1); }
    else if (i.op === O.REPEAT_REG) { s.push(pc + 1); if (end.get(pc) + 1 < code.length) s.push(end.get(pc) + 1); }
    else if (i.op === O.BREAK) { s.push(pc + 1); const e = end.get(inner[inner.length - 1]); if (e + 1 < code.length) s.push(e + 1); }
    else if (pc + 1 < code.length) s.push(pc + 1);
    if (i.op === O.REPEAT || i.op === O.REPEAT_REG) inner.push(pc);
    if (i.op === O.END) inner.pop();
    out.push(s.filter(x => x < code.length));
  });
  return out;
}
// Registers (or vregs) live after each instruction; loops iterate to a fixpoint.
function liveness(code) {
  const succ = successors(code), n = code.length;
  const liveIn = Array.from({length: n}, () => new Set()), liveOut = Array.from({length: n}, () => new Set());
  for (let changed = true; changed;) {
    changed = false;
    for (let pc = n - 1; pc >= 0; --pc) {
      const out = new Set();
      for (const s of succ[pc]) for (const v of liveIn[s]) out.add(v);
      const i = code[pc], inn = new Set(out);
      if (defines(i)) inn.delete(i.dst);
      for (const u of uses(i)) inn.add(u);
      if (inn.size !== liveIn[pc].size || out.size !== liveOut[pc].size) changed = true;
      liveIn[pc] = inn; liveOut[pc] = out;
    }
  }
  return {liveIn, liveOut};
}
function removeDead(code) {
  let removed = 0;
  for (;;) {
    const {liveOut} = liveness(code);
    const keep = code.filter((i, pc) => !defines(i) || liveOut[pc].has(i.dst));
    if (keep.length === code.length) return {code, removed};
    removed += code.length - keep.length; code = keep;
  }
}

// ------------------------------------------------------------------- lexer
function lex(src) {
  const t = [], re = /\s+|\/\/[^\n]*|\/\*[\s\S]*?\*\/|(0x[0-9a-fA-F]+|(?:\d+\.?\d*|\.\d+)(?:e[-+]?\d+)?)|(\$\d+)|([A-Za-z_]\w*)|(\+=|-=|\*=|[-+*\/(){},;=<>])/y;
  let m;
  for (re.lastIndex = 0; re.lastIndex < src.length;) {
    const at = re.lastIndex;
    if (!(m = re.exec(src))) throw new SyntaxError(`bad character at ${src.slice(at, at + 20)}`);
    const line = src.slice(0, at).split('\n').length;
    if (m[1]) t.push({k: 'num', v: m[1], line});
    else if (m[2]) t.push({k: 'par', v: +m[2].slice(1), line});
    else if (m[3]) t.push({k: 'id', v: m[3], line});
    else if (m[4]) t.push({k: m[4], line});
  }
  t.push({k: 'eof', line: -1});
  return t;
}
// ------------------------------------------------------------------ parser
function parse(src) {
  const t = lex(src);
  let p = 0;
  const peek = k => t[p].k === k || (t[p].k === 'id' && t[p].v === k);
  const take = k => { if (!peek(k)) throw new SyntaxError(`line ${t[p].line}: expected ${k}, got ${t[p].v ?? t[p].k}`); return t[p++]; };
  const id = () => take('id').v;
  function primary() {
    const x = t[p];
    if (x.k === 'num') { p++; return {k: 'num', lit: x.v}; }
    if (x.k === 'par') { p++; return {k: 'par', n: x.v}; }
    if (x.k === '(') { p++; const e = expr(); take(')'); return e; }
    if (x.k === 'id') {
      p++;
      if (!peek('(')) return {k: 'name', v: x.v, line: x.line};
      take('('); const args = [];
      if (!peek(')')) do args.push(expr()); while (peek(',') && take(','));
      take(')');
      return {k: 'call', f: x.v, args, line: x.line};
    }
    throw new SyntaxError(`line ${x.line}: unexpected ${x.v ?? x.k}`);
  }
  function unary() { if (peek('-')) { p++; return {k: 'neg', a: unary()}; } return primary(); }
  function term() {
    let a = unary();
    while (peek('*') || peek('/')) { const o = t[p++].k; a = {k: o, a, b: unary()}; }
    return a;
  }
  function expr() {
    let a = term();
    while (peek('+') || peek('-')) { const o = t[p++].k; a = {k: o, a, b: term()}; }
    return a;
  }
  function block() { take('{'); const s = []; while (!peek('}')) s.push(stmt()); take('}'); return s; }
  function stmt() {
    const line = t[p].line;
    if (peek('input')) {
      p++; const d = [];
      do { const n = id(); take('='); d.push({n, k: +take('num').v}); } while (peek(',') && take(','));
      take(';'); return {k: 'input', d, line};
    }
    if (peek('let') || peek('const')) {
      const kind = t[p++].v, d = [];
      do { const n = id(); take('='); d.push({n, e: expr()}); } while (peek(',') && take(','));
      take(';'); return {k: 'decl', kind, d, line};
    }
    if (peek('repeat')) { p++; take('('); const n = expr(); take(')'); return {k: 'repeat', n, body: block(), line}; }
    // unroll (N) { ... }: the body N times at compile time (no VM loop).
    if (peek('unroll')) { p++; take('('); const n = +take('num').v; take(')'); return {k: 'unroll', n, body: block(), line}; }
    // function f(a, b) { return e; }: an expression macro, inlined per call.
    if (peek('function')) {
      p++; const name = id(), params = []; take('(');
      if (!peek(')')) do params.push(id()); while (peek(',') && take(','));
      take(')'); take('{'); take('return'); const e = expr(); take(';'); take('}');
      return {k: 'func', name, params, e, line};
    }
    if (peek('if')) {
      p++; take('('); const a = expr(), o = peek('>') ? take('>').k : take('<').k, b = expr(); take(')');
      take('break'); take(';');
      return {k: 'break', a: o === '>' ? a : b, b: o === '>' ? b : a, line};
    }
    const n = id();
    if (peek('(')) {
      take('('); const args = [];
      if (!peek(')')) do args.push(expr()); while (peek(',') && take(','));
      take(')'); take(';');
      return {k: 'draw', f: n, args, line};
    }
    const o = t[p].k;
    if (!['=', '+=', '-=', '*='].includes(o)) throw new SyntaxError(`line ${line}: expected assignment`);
    p++; const e = expr(); take(';');
    return {k: 'assign', n, o, e, line};
  }
  const body = [];
  while (!peek('eof')) body.push(stmt());
  return body;
}

// --------------------------------------------------------------- lowering
function lower(ast) {
  const vregs = [], code = [], names = new Map(), warnings = [];
  const assigned = new Set();
  (function scan(s) { for (const x of s) { if (x.k === 'assign') assigned.add(x.n); if (x.body) scan(x.body); } })(ast);
  const pools = new Map(), pinned = new Map(), funcs = new Map();
  let cse = new Map();
  const newReg = (kind, name) => (vregs.push({kind, name, pin: null}), vregs.length - 1);
  const key = x => x.t === 'c' ? 'c' + (Object.is(x.v, -0) ? '-0' : x.v) : x.t === 'p' ? 'p' + x.n : x.t === 'i' ? 'i' + x.k
    : 'r' + x.vr + '@' + (x.ver ?? 0);
  const cst = (v, lit) => ({t: 'c', v: f32(v), lit: lit ?? shortest(f32(v))});
  function pool(x, pin = '') {
    const k = key(x) + pin;
    if (!pools.has(k)) {
      const vr = newReg('pool', k);
      vregs[vr].def = x.t === 'i' ? {op: O.INPUT, a: x.k} : x.t === 'p' ? {op: O.SET, par: {value: x.n}}
        : {op: O.SET, value: x.v, lit: x.lit};
      pools.set(k, vr);
    }
    return pools.get(k);
  }
  const mat = x => x.t === 'r' ? x.vr : pool(x);
  const varOf = n => { const b = names.get(n); if (!b || b.kind !== 'var') throw new Error(`${n} is not a variable`); return b; };
  function readName(n, line) {
    const b = names.get(n);
    if (!b) throw new Error(`line ${line}: ${n} is not defined`);
    if (b.kind === 'alias') return b.x;
    return {t: 'r', vr: b.vr, ver: b.ver, mut: true};
  }
  function emit(i) { code.push(i); return i; }
  function written(b) { b.ver++; }
  // op with operands already evaluated; target is a var binding or null.
  function op2(op, A, B, target) {
    if (op !== O.SIN && A.t === 'c' && B.t === 'c') {
      const v = op === O.ADD ? f32(A.v + B.v) : f32(A.v * B.v);
      return target ? setTo(target, cst(v)) : cst(v);
    }
    if (op === O.MUL && B.t === 'c' && B.v === 1 && !target) return A;
    if (op === O.MUL && A.t === 'c' && A.v === 1 && !target) return B;
    const k = op === O.SIN ? 'N' + key(A) : OPS[op] + [key(A), key(B)].sort().join(',');
    if (!target && cse.has(k)) {
      const h = cse.get(k);
      if (!h.b || h.b.ver === h.x.ver) return h.x;
    }
    const a = mat(A), b = op === O.SIN ? 0 : mat(B);
    if (target) {
      emit({op, dst: target.vr, a, b}); written(target);
      const x = {t: 'r', vr: target.vr, ver: target.ver, mut: true};
      cse.set(k, {x, b: target}); return x;
    }
    const dst = newReg('temp');
    emit({op, dst, a, b});
    const x = {t: 'r', vr: dst};
    cse.set(k, {x}); return x;
  }
  function setTo(b, x) {
    if (x.t === 'c') emit({op: O.SET, dst: b.vr, value: x.v, lit: x.lit});
    else if (x.t === 'p') emit({op: O.SET, dst: b.vr, value: 0, par: {value: x.n}});
    else if (x.t === 'i') emit({op: O.INPUT, dst: b.vr, a: x.k});
    else if (x.vr !== b.vr) emit({op: O.ADD, dst: b.vr, a: x.vr, b: pool(cst(0)), copy: x.vr});
    else return x;
    written(b);
    return {t: 'r', vr: b.vr, ver: b.ver, mut: true};
  }
  function ev(e, target = null) {
    switch (e.k) {
      case 'num': { const x = cst(Number(e.lit), e.lit.replace(/^0\./, '.')); return target ? setTo(target, x) : x; }
      case 'par': { const x = {t: 'p', n: e.n}; return target ? setTo(target, x) : x; }
      case 'name': { const x = readName(e.v, e.line); return target ? setTo(target, x) : x; }
      case '+': return op2(O.ADD, ev(e.a), ev(e.b), target);
      case '*': return op2(O.MUL, ev(e.a), ev(e.b), target);
      case '-': {
        const A = ev(e.a), B = ev(e.b);
        if (B.t === 'c') return op2(O.ADD, A, cst(-B.v), target);
        return op2(O.ADD, A, op2(O.MUL, B, cst(-1)), target);
      }
      case '/': {
        const A = ev(e.a), B = ev(e.b);
        if (B.t !== 'c') throw new Error('division only by a constant');
        const r = f32(1 / B.v);
        if (f32(1 / r) !== B.v || Math.log2(Math.abs(B.v)) % 1) warnings.push(`x/${B.lit} compiled as x*${shortest(r)} (not exact)`);
        return op2(O.MUL, A, cst(r), target);
      }
      case 'neg': {
        const A = ev(e.a);
        if (A.t === 'c') { const x = cst(-A.v); return target ? setTo(target, x) : x; }
        return op2(O.MUL, A, cst(-1), target);
      }
      case 'call':
        if (e.f === 'sin') return op2(O.SIN, ev(e.args[0]), null, target);
        if (e.f === 'in') { const x = {t: 'i', k: +e.args[0].lit}; return target ? setTo(target, x) : x; }
        if (funcs.has(e.f)) {
          const f = funcs.get(e.f);
          if (f.params.length !== e.args.length) throw new Error(`line ${e.line}: ${e.f} takes ${f.params.length} arguments`);
          const xs = e.args.map(a => ev(a)), saved = f.params.map(n => names.get(n));
          f.params.forEach((n, k) => names.set(n, {kind: 'alias', x: xs[k]}));
          try { return ev(f.e, target); } finally { f.params.forEach((n, k) => saved[k] ? names.set(n, saved[k]) : names.delete(n)); }
        }
        throw new Error(`line ${e.line}: unknown function ${e.f}`);
    }
    throw new Error(`bad expression ${e.k}`);
  }
  const imm = (x, what) => {
    if (x.t === 'p') return {par: x.n};
    if (x.t !== 'c' || !Number.isInteger(x.v)) throw new Error(`${what} must be an integer constant or $n`);
    return {v: x.v};
  };
  function stmts(list) {
    for (const s of list) {
      switch (s.k) {
        case 'input': for (const d of s.d) names.set(d.n, {kind: 'alias', x: {t: 'i', k: d.k}}); break;
        case 'func': funcs.set(s.name, s); break;
        // A registration argument by name (a plan function's parameter): $n.
        case 'param': names.set(s.n, {kind: 'alias', x: {t: 'p', n: s.index}}); break;
        case 'unroll': for (let k = 0; k < s.n; ++k) stmts(s.body); break;
        case 'decl':
          for (const d of s.d) {
            if (assigned.has(d.n)) {
              if (s.kind === 'const') throw new Error(`line ${s.line}: const ${d.n} is assigned`);
              let b = names.get(d.n);
              if (!b || b.kind !== 'var') { b = {kind: 'var', vr: newReg('var', d.n), ver: 0}; }
              ev(d.e, b); names.set(d.n, b);
            } else {
              let x = ev(d.e);
              // A snapshot of a variable must not follow its later writes.
              if (x.mut) { const b = {kind: 'var', vr: newReg('var', d.n), ver: 0}; setTo(b, x); x = {t: 'r', vr: b.vr}; }
              names.set(d.n, {kind: 'alias', x});
            }
          }
          break;
        case 'assign': {
          const b = varOf(s.n);
          if (s.o === '=') ev(s.e, b);
          else ev({k: s.o[0], a: {k: 'name', v: s.n}, b: s.e}, b);
          break;
        }
        case 'repeat': {
          const n = ev(s.n);
          if (n.t === 'c' || n.t === 'p') {
            const c = imm(n, 'repeat count');
            if (c.v !== undefined && (c.v < 1 || c.v > 255)) throw new Error(`line ${s.line}: repeat 1..255`);
            emit(c.par !== undefined ? {op: O.REPEAT, a: 0, par: {a: c.par}} : {op: O.REPEAT, a: c.v});
          } else emit({op: O.REPEAT_REG, a: mat(n)});
          cse = new Map(); stmts(s.body); emit({op: O.END}); cse = new Map();
          break;
        }
        case 'break': emit({op: O.BREAK, a: mat(ev(s.a)), b: mat(ev(s.b))}); break;
        case 'draw': {
          const f = s.f, A = s.args;
          if (f === 'cubic') {
            if (A.length !== 10) throw new Error(`line ${s.line}: cubic(segments, color, x0,y0,x1,y1,x2,y2,x3,y3)`);
            const n = imm(ev(A[0]), 'segments'), c = imm(ev(A[1]), 'color');
            // CUBIC reads r0..r7: a value already bound to another point gets
            // its own register (a constant or input is set again, anything
            // else copied), as the hand IR does with S0,120 ... S2,120.
            const args = A.slice(2).map((e, k) => {
              const x = ev(e), v = mat(x);
              if (!pinned.has(v) || pinned.get(v) === k) { pinned.set(v, k); return v; }
              // A constant or input gets a second pool entry of its own (still
              // hoisted out of loops); a computed value is copied here.
              if (x.t !== 'r') { const t = pool(x, '@r' + k); pinned.set(t, k); return t; }
              const t = newReg('temp');
              emit({op: O.ADD, dst: t, a: v, b: pool(cst(0)), copy: v});
              pinned.set(t, k); return t;
            });
            const i = {op: O.CUBIC, dst: 0, a: n.v ?? 0, color: c.v ?? 0, par: {}, args};
            if (n.par !== undefined) i.par.a = n.par;
            if (c.par !== undefined) i.par.color = c.par;
            emit(i); break;
          }
          const op = {move: O.MOVE, plot: O.PLOT, line: O.LINE}[f];
          if (op === undefined) throw new Error(`line ${s.line}: unknown statement ${f}`);
          const x = mat(ev(A[0])), y = mat(ev(A[1]));
          if (op === O.MOVE) { emit({op, a: x, b: y}); break; }
          const C = ev(A[2]);
          if (C.t === 'c' || C.t === 'p') {
            const c = imm(C, 'color');
            emit(c.par !== undefined ? {op, a: x, b: y, color: 0, par: {color: c.par}} : {op, a: x, b: y, color: c.v});
          } else emit({op: op === O.PLOT ? O.PLOTC : O.LINEC, dst: mat(C), a: x, b: y});
          break;
        }
      }
    }
  }
  stmts(ast);
  for (const i of code) { i.par ??= {}; i.value ??= 0; i.color ??= 0; i.dst ??= 0; i.a ??= 0; i.b ??= 0; }
  return {code, vregs, warnings};
}

// Pool values (constants, parameters, inputs) are defined once, before the
// top-level statement of their first use: outside every loop.
function placePools(code, vregs) {
  const first = new Map();
  let depth = 0, top = 0;
  code.forEach((i, pc) => {
    if (depth === 0) top = pc;
    for (const u of uses(i)) if (vregs[u].kind === 'pool' && !first.has(u)) first.set(u, top);
    if (i.op === O.REPEAT || i.op === O.REPEAT_REG) depth++;
    if (i.op === O.END) depth--;
  });
  const at = [...first].sort((a, b) => b[1] - a[1] || b[0] - a[0]);
  for (const [vr, pc] of at) code.splice(pc, 0, {...vregs[vr].def, dst: vr, par: {...(vregs[vr].def.par ?? {})},
    value: vregs[vr].def.value ?? 0, a: vregs[vr].def.a ?? 0, b: 0, color: 0, pool: true});
  return code;
}
// Re-materialise one pool value at each use (shorter live range, more code).
function remat(code, vregs, vr) {
  const out = [];
  for (const i of code) {
    if (defines(i) && i.dst === vr) continue;
    if (uses(i).includes(vr)) {
      const t = vregs.push({kind: 'remat', name: vregs[vr].name, pin: null}) - 1;
      out.push({...vregs[vr].def, dst: t, par: {...(vregs[vr].def.par ?? {})}, value: vregs[vr].def.value ?? 0,
        a: vregs[vr].def.a ?? 0, b: 0, color: 0});
      const sub = x => x === vr ? t : x;
      const j = {...i};
      if (j.op === O.CUBIC) j.args = j.args.map(sub);
      else { for (const f of ['a', 'b']) if (uses(i).length && j[f] === vr && j.op !== O.REPEAT) j[f] = t; if ((j.op === O.PLOTC || j.op === O.LINEC) && j.dst === vr) j.dst = t; }
      out.push(j);
    } else out.push(i);
  }
  vregs[vr].gone = true;
  return out;
}
function color(code, vregs) {
  const {liveOut, liveIn} = liveness(code), adj = new Map(), nodes = new Set();
  const edge = (a, b) => { if (a === b) return; (adj.get(a) ?? adj.set(a, new Set()).get(a)).add(b); (adj.get(b) ?? adj.set(b, new Set()).get(b)).add(a); };
  code.forEach((i, pc) => {
    for (const u of uses(i)) nodes.add(u);
    if (defines(i)) { nodes.add(i.dst); for (const v of liveOut[pc]) if (!(i.copy === v)) edge(i.dst, v); }
  });
  // Values live together at a CUBIC must also hold distinct registers.
  code.forEach((i, pc) => {
    if (i.op !== O.CUBIC) return;
    const live = new Set([...liveIn[pc]]);
    for (const a of live) for (const b of live) if (a < b) edge(a, b);
  });
  let pressure = 0, at = 0;
  liveIn.forEach((s, pc) => { if (s.size > pressure) { pressure = s.size; at = pc; } });
  // Pins (CUBIC reads r0..r7) first, then Chaitin's order.
  const pin = new Map();
  code.forEach(i => { if (i.op === O.CUBIC) i.args.forEach((v, k) => pin.set(v, pin.has(v) && pin.get(v) !== k ? -1 : k)); });
  for (const [v, k] of pin) if (k < 0) return {fail: `value used as two CUBIC points: ${vregs[v].name ?? v}`};
  for (const [v, k] of pin) for (const w of adj.get(v) ?? []) if (pin.get(w) === k) return {fail: `CUBIC r${k} is wanted by two live values`, pinConflict: [v, w]};
  const deg = v => [...(adj.get(v) ?? [])].filter(w => rest.has(w)).length;
  const rest = new Set([...nodes].filter(v => !pin.has(v))), stack = [];
  while (rest.size) {
    let pick = null;
    for (const v of rest) if (deg(v) < REGS) { pick = v; break; }
    if (pick === null) pick = [...rest].sort((a, b) => deg(b) - deg(a))[0];
    rest.delete(pick); stack.push(pick);
  }
  const reg = new Map(pin);
  const copyOf = new Map();
  code.forEach(i => { if (i.copy !== undefined) { copyOf.set(i.dst, i.copy); copyOf.set(i.copy, i.dst); } });
  while (stack.length) {
    const v = stack.pop(), taken = new Set([...(adj.get(v) ?? [])].map(w => reg.get(w)));
    const want = reg.get(copyOf.get(v));
    let r = want !== undefined && !taken.has(want) ? want : -1;
    // The zero constant takes the top free register and everything else the
    // bottom one, so zero usually lands where nothing wrote and its SET goes.
    const zero = vregs[v].kind === 'pool' && vregs[v].name === 'c0';
    if (r < 0) for (let n = 0; n < REGS; ++n) { const k = zero ? REGS - 1 - n : n; if (!taken.has(k)) { r = k; break; } }
    if (r < 0) return {fail: `more than ${REGS} values live (${pressure} at pc ${at})`, pressure};
    reg.set(v, r);
  }
  return {reg, pressure};
}
function emitPhysical(code, reg) {
  const R = v => reg.get(v);
  let out = code.map(i => {
    const j = {op: i.op, dst: 0, a: i.a, b: i.b, value: i.value ?? 0, lit: i.lit ?? null, color: i.color ?? 0, par: {...i.par}};
    if (defines(i)) j.dst = R(i.dst);
    if ([O.ADD, O.MUL].includes(i.op)) { j.a = R(i.a); j.b = R(i.b); }
    if ([O.SIN, O.REPEAT_REG].includes(i.op)) j.a = R(i.a);
    if ([O.MOVE, O.PLOT, O.LINE, O.BREAK].includes(i.op)) { j.a = R(i.a); j.b = R(i.b); }
    if ([O.PLOTC, O.LINEC].includes(i.op)) { j.dst = R(i.dst); j.a = R(i.a); j.b = R(i.b); }
    if (i.op === O.SET && !('value' in j.par) && j.lit == null) j.lit = shortest(f32(j.value));
    if (i.copy !== undefined && R(i.copy) === j.dst) j.drop = true;
    return j;
  }).filter(j => !j.drop);
  // Registers start at +0 each draw: a top-level SET 0 before any write to
  // its register is already true.
  const written = new Set();
  let depth = 0;
  out = out.filter(j => {
    const zero = j.op === O.SET && !('value' in j.par) && Object.is(f32(+j.lit), 0);
    const drop = zero && depth === 0 && !written.has(j.dst);
    if (defines(j)) written.add(j.dst);
    if (j.op === O.REPEAT || j.op === O.REPEAT_REG) depth++;
    if (j.op === O.END) depth--;
    return !drop;
  });
  return out;
}

export function compile(src) { return compileAst(parse(src)); }
// ast: the statements parse() makes (plan_js.mjs builds the same from a JS
// function).
export function compileAst(ast) {
  const {code: lowered, vregs, warnings} = lower(ast);
  let code = placePools(lowered, vregs);
  let dead = 0, rematerialised = [];
  ({code, removed: dead} = removeDead(code));
  let r;
  for (;;) {
    r = color(code, vregs);
    if (!r.fail) break;
    if (r.pinConflict) {
      // Give the later-defined pinned value its own copy at the CUBIC.
      throw new Error(r.fail);
    }
    const counts = new Map();
    code.forEach(i => { for (const u of uses(i)) if (vregs[u].kind === 'pool') counts.set(u, (counts.get(u) ?? 0) + 1); });
    const pick = [...counts].sort((a, b) => a[1] - b[1])[0];
    if (!pick) throw new Error(r.fail);
    rematerialised.push(vregs[pick[0]].name);
    code = remat(code, vregs, pick[0]);
    ({code} = removeDead(code));
  }
  const out = emitPhysical(code, r.reg);
  // register() takes 1..64 instructions: a plan that draws nothing of its own
  // (it carries typed points, DERBY's 'S0,0') is one SET.
  if (!out.length) out.push({op: O.SET, dst: 0, a: 0, b: 0, value: 0, lit: '0', color: 0, par: {}});
  return {code: out, text: formatText(out), count: out.length, dead, rematerialised, warnings, pressure: r.pressure};
}

// ------------------------------------------------------------- decompiler
// The hand IR as the language: one variable per web (the defs that reach a
// common use), constants and inputs inlined, single-use temporaries folded
// into the expression that reads them when nothing in between can change them.
export function decompile(src) {
  const code = parseText(src), n = code.length, succ = successors(code);
  const pred = Array.from({length: n}, () => []);
  succ.forEach((s, pc) => s.forEach(x => pred[x].push(pc)));
  const regsOf = i => i.op === O.CUBIC ? [0, 1, 2, 3, 4, 5, 6, 7] : uses(i);
  // reaching definitions: def id = pc, entry defs = -1 - r
  const IN = Array.from({length: n}, () => new Map()), OUT = Array.from({length: n}, () => new Map());
  const entry = new Map([...Array(REGS).keys()].map(r => [r, new Set([-1 - r])]));
  for (let changed = true; changed;) {
    changed = false;
    for (let pc = 0; pc < n; ++pc) {
      const inn = new Map();
      const srcs = pc === 0 ? [entry] : [];
      for (const p of pred[pc]) srcs.push(OUT[p]);
      for (const m of srcs) for (const [r, s] of m) { if (!inn.has(r)) inn.set(r, new Set()); for (const d of s) inn.get(r).add(d); }
      const out = new Map([...inn].map(([r, s]) => [r, new Set(s)]));
      if (defines(code[pc])) out.set(code[pc].dst, new Set([pc]));
      const size = m => [...m.values()].reduce((a, s) => a + s.size, 0);
      if (size(inn) !== size(IN[pc]) || size(out) !== size(OUT[pc])) changed = true;
      IN[pc] = inn; OUT[pc] = out;
    }
  }
  const parent = new Map(), find = x => { while (parent.get(x) !== x) x = parent.get(x); return x; };
  const add = x => { if (!parent.has(x)) parent.set(x, x); };
  const union = (a, b) => { add(a); add(b); parent.set(find(a), find(b)); };
  code.forEach((i, pc) => { if (defines(i)) add(pc); });
  const useDefs = new Map();
  code.forEach((i, pc) => {
    for (const r of regsOf(i)) {
      const ds = [...(IN[pc].get(r) ?? [])];
      ds.forEach(add);
      for (let k = 1; k < ds.length; ++k) union(ds[0], ds[k]);
      useDefs.set(pc + ':' + r, ds);
    }
  });
  const webDefs = new Map(), webUses = new Map();
  for (const x of parent.keys()) { const w = find(x); if (!webDefs.has(w)) webDefs.set(w, []); webDefs.get(w).push(x); }
  code.forEach((i, pc) => {
    for (const r of regsOf(i)) { const w = find(useDefs.get(pc + ':' + r)[0]); if (!webUses.has(w)) webUses.set(w, []); webUses.get(w).push(pc); }
  });
  const regOfWeb = w => { const d = webDefs.get(w)[0]; return d < 0 ? -1 - d : code[d].dst; };
  const names = new Map(), perReg = new Map();
  for (const w of webDefs.keys()) {
    const r = regOfWeb(w), k = perReg.get(r) ?? 0; perReg.set(r, k + 1);
    names.set(w, 'r' + r + (k ? String.fromCharCode(97 + k) : ''));
  }
  const webAt = (pc, r) => find(useDefs.get(pc + ':' + r)[0]);
  const single = w => webDefs.get(w).length === 1 && webDefs.get(w)[0] >= 0;
  const lit = i => ('value' in i.par ? '$' + i.par.value : i.lit);
  // inline: constants/inputs/params with one def; arithmetic with one use in reach
  // leaves: the registers an inlined expression reads when it is evaluated at
  // its use instead of its def; none of them may change in between.
  const inline = new Map(), leaves = new Map();
  const webs = [...webDefs.keys()].filter(single).sort((a, b) => webDefs.get(a)[0] - webDefs.get(b)[0]);
  for (const w of webs) {
    const d = webDefs.get(w)[0], i = code[d], us = webUses.get(w) ?? [];
    if (!us.length) continue;
    if (i.op === O.SET || i.op === O.INPUT) { inline.set(w, true); leaves.set(w, new Set()); continue; }
    if (us.length !== 1) continue;
    const u = us[0], read = new Set();
    for (const r of uses(i)) { const v = webAt(d, r); for (const x of inline.has(v) ? leaves.get(v) : [r]) read.add(x); }
    let ok = u > d;
    for (let pc = d + 1; ok && pc < u; ++pc) {
      const j = code[pc];
      if ([O.REPEAT, O.REPEAT_REG, O.END, O.BREAK].includes(j.op)) ok = false;
      else if (defines(j) && read.has(j.dst)) ok = false;
    }
    if (ok && code[u].op === O.CUBIC) ok = false;
    if (ok) { inline.set(w, true); leaves.set(w, read); }
  }
  function operand(pc, r, prec = 0) {
    const w = webAt(pc, r);
    if (!inline.has(w)) return names.get(w);
    return expr(webDefs.get(w)[0], prec);
  }
  function expr(d, prec = 0) {
    const i = code[d];
    const wrap = (s, p) => p < prec ? '(' + s + ')' : s;
    const neg = (pc, r) => {
      const w = webAt(pc, r);
      if (!inline.has(w)) return null;
      const j = code[webDefs.get(w)[0]];
      if (j.op !== O.MUL) return null;
      const one = (x) => { const v = webAt(webDefs.get(w)[0], x); const k = code[webDefs.get(v)[0]]; return inline.has(v) && k?.op === O.SET && !('value' in k.par) && +k.lit === -1; };
      if (one(j.b)) return operand(webDefs.get(w)[0], j.a, 2);
      if (one(j.a)) return operand(webDefs.get(w)[0], j.b, 3);
      return null;
    };
    switch (i.op) {
      case O.SET: return lit(i).startsWith('-') && prec > 1 ? '(' + lit(i) + ')' : lit(i);
      case O.INPUT: return 'in(' + i.a + ')';
      case O.SIN: return 'sin(' + operand(d, i.a) + ')';
      case O.ADD: {
        const nb = neg(d, i.b);
        if (nb !== null) return wrap(operand(d, i.a, 1) + ' - ' + nb, 1);
        return wrap(operand(d, i.a, 1) + ' + ' + operand(d, i.b, 2), 1);
      }
      case O.MUL: return wrap(operand(d, i.a, 2) + ' * ' + operand(d, i.b, 3), 2);
    }
    throw new Error('not an expression');
  }
  const out = [], declared = new Set();
  const zeroInit = [...webDefs.keys()].filter(w => webDefs.get(w).some(d => d < 0) && (webUses.get(w) ?? []).length);
  for (const w of zeroInit) { out.push(`let ${names.get(w)} = 0;`); declared.add(w); }
  let ind = '';
  code.forEach((i, pc) => {
    const o = (r) => operand(pc, r);
    switch (i.op) {
      case O.REPEAT: out.push(`${ind}repeat (${'a' in i.par ? '$' + i.par.a : i.a}) {`); ind += '  '; return;
      case O.REPEAT_REG: out.push(`${ind}repeat (${o(i.a)}) {`); ind += '  '; return;
      case O.END: ind = ind.slice(2); out.push(`${ind}}`); return;
      case O.BREAK: out.push(`${ind}if (${o(i.a)} > ${o(i.b)}) break;`); return;
      case O.MOVE: out.push(`${ind}move(${o(i.a)}, ${o(i.b)});`); return;
      case O.PLOT: case O.LINE: out.push(`${ind}${i.op === O.PLOT ? 'plot' : 'line'}(${o(i.a)}, ${o(i.b)}, ${'color' in i.par ? '$' + i.par.color : i.color});`); return;
      case O.PLOTC: case O.LINEC: out.push(`${ind}${i.op === O.PLOTC ? 'plot' : 'line'}(${o(i.a)}, ${o(i.b)}, ${o(i.dst)});`); return;
      case O.CUBIC: out.push(`${ind}cubic(${'a' in i.par ? '$' + i.par.a : i.a}, ${'color' in i.par ? '$' + i.par.color : i.color}, ${[0, 1, 2, 3, 4, 5, 6, 7].map(o).join(', ')});`); return;
    }
    const w = find(pc);
    if (inline.has(w)) return;
    const dead = !(webUses.get(w) ?? []).length;
    let e = expr(pc);
    // x = x + e and x = x * e read better as x += e
    const nm = names.get(w);
    if (declared.has(w) && (i.op === O.ADD || i.op === O.MUL) && operand(pc, i.a) === nm && !inline.has(webAt(pc, i.a))) {
      const s = expr(pc);
      out.push(`${ind}${nm} ${s[nm.length + 1]}= ${s.slice(nm.length + 3)};${dead ? ' // dead' : ''}`);
      return;
    }
    const decl = declared.has(w) ? '' : (webDefs.get(w).length > 1 ? 'let ' : 'const ');
    declared.add(w);
    out.push(`${ind}${decl}${nm} = ${e};${dead ? ' // dead' : ''}`);
  });
  return out.join('\n') + '\n';
}

export function stats(code) {
  const regs = new Set();
  for (const i of code) { if (defines(i)) regs.add(i.dst); for (const u of uses(i)) regs.add(u); }
  return {count: code.length, regs: regs.size};
}
export {liveness, removeDead, successors};
