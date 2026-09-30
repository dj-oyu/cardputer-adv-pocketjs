// Plans written as ordinary JS functions, compiled to the procedural IR by
// kir.mjs, and the host reference that runs the same functions. Host only
// (Node), no dependencies. docs/kasane/js-to-ir.md section 5.
//
//   /** @plan rail inputs: x0, dx, top, ground, posts, mid, colour */
//   function rail() {
//     let x = x0;
//     for (let i = 0; i < posts; i++) { move(x, top); line(x, ground, colour); x += dx; }
//   }
//
// The names after "inputs:" are the draw inputs 0..7 in order; the
// function's parameters are the registration arguments $0, $1, ... Outside
// the subset (RULES below) is a compile error naming the line and the rule.
import {compileAst} from './kir.mjs';

export const RULES = {
  R1: 'statements: let/const, = += -= *=, for (let i = 0; i < n; i++), if (a > b) break;, move/line/plot/cubic(...)',
  R2: 'expressions: numbers, names, + - *, unary -, / by a constant, sin() or Math.sin(), Math.PI, parentheses',
  R3: 'loops: for (let i = 0; i < n; i++) with n not changed in the body and i not assigned; no while/do/continue',
  R4: 'names: the @plan inputs, the parameters, locals declared before use; no shadowing, no reassigned const',
  R5: 'no strings, arrays, objects, other calls, ternaries, comparisons outside if/for, % ** or bit operators',
  R6: 'break only as if (a > b) break; or if (a < b) break; (the VM has BREAK_IF_GT) inside a for',
};
class PlanError extends SyntaxError {}
const fail = (line, rule, what) => { throw new PlanError(`line ${line}: ${what} [${rule}: ${RULES[rule]}]`); };

function lex(src, line0) {
  const t = [], re = /\s+|\/\/[^\n]*|\/\*[\s\S]*?\*\/|(0x[0-9a-fA-F]+|(?:\d+\.?\d*|\.\d+)(?:e[-+]?\d+)?)|([A-Za-z_$][\w$]*)|(\+\+|--|\+=|-=|\*=|\/=|%=|\*\*|===|!==|==|!=|<=|>=|&&|\|\||=>|[-+*\/%(){}\[\],;=<>.!?:&|^~'"`])/y;
  let m, line = line0;
  for (re.lastIndex = 0; re.lastIndex < src.length;) {
    const at = re.lastIndex;
    if (!(m = re.exec(src))) fail(line, 'R5', `unexpected ${JSON.stringify(src[at])}`);
    if (m[1]) t.push({k: 'num', v: m[1], line});
    else if (m[2]) t.push({k: 'id', v: m[2], line});
    else if (m[3]) t.push({k: m[3], line});
    line += (m[0].match(/\n/g) ?? []).length;
  }
  t.push({k: 'eof', line});
  return t;
}
const KEYWORDS = new Set(['while', 'do', 'switch', 'return', 'var', 'function', 'try', 'throw', 'new', 'class',
  'continue', 'else', 'yield', 'await', 'typeof', 'delete', 'this', 'with', 'in', 'of', 'import', 'export']);

// The function body -> kir.mjs statements (for desugared to repeat).
function parseBody(src, line0, inputs, params) {
  const t = lex(src, line0);
  let p = 0;
  const scopes = [new Map()];
  const lookup = n => { for (let s = scopes.length - 1; s >= 0; --s) if (scopes[s].has(n)) return scopes[s].get(n); return null; };
  const declare = (n, kind, line) => {
    if (KEYWORDS.has(n)) fail(line, 'R4', `${n} is a keyword`);
    if (lookup(n)) fail(line, 'R4', `${n} shadows a name already in scope`);
    scopes[scopes.length - 1].set(n, {kind});
  };
  inputs.forEach(n => declare(n, 'input', line0));
  params.forEach(n => declare(n, 'param', line0));
  const peek = (k, o = 0) => t[p + o].k === k || (t[p + o].k === 'id' && t[p + o].v === k);
  const take = (k, rule = 'R1') => { if (!peek(k)) fail(t[p].line, rule, `expected ${k}, got ${t[p].v ?? t[p].k}`); return t[p++]; };
  function primary() {
    const x = t[p];
    if (x.k === 'num') { p++; return {k: 'num', lit: x.v}; }
    if (x.k === '(') { p++; const e = expr(); take(')', 'R2'); return e; }
    if (x.k === 'id' && x.v === 'Math' && peek('.', 1)) {
      p += 2; const f = take('id', 'R2').v;
      if (f === 'PI') return {k: 'num', lit: String(Math.PI)};
      if (f !== 'sin') fail(x.line, 'R2', `Math.${f}`);
      return call('sin', x.line);
    }
    if (x.k === 'id') {
      p++;
      if (peek('(')) {
        if (x.v !== 'sin') fail(x.line, 'R5', `a call to ${x.v}() in an expression`);
        return call('sin', x.line);
      }
      if (KEYWORDS.has(x.v)) fail(x.line, 'R5', `${x.v}`);
      if (!lookup(x.v)) fail(x.line, 'R4', `${x.v} is not declared here`);
      return {k: 'name', v: x.v, line: x.line};
    }
    fail(x.line, x.k === '[' || x.k === "'" || x.k === '"' || x.k === '`' || x.k === '{' ? 'R5' : 'R2', `unexpected ${x.v ?? x.k}`);
  }
  function call(f, line) {
    take('(', 'R2'); const a = expr(); take(')', 'R2');
    return {k: 'call', f, args: [a], line};
  }
  function unary() {
    if (peek('-')) { p++; return {k: 'neg', a: unary()}; }
    if (peek('+') || peek('!') || peek('~') || peek('++') || peek('--')) fail(t[p].line, 'R2', `unary ${t[p].k}`);
    return primary();
  }
  function term() {
    let a = unary();
    while (peek('*') || peek('/')) { const o = t[p++].k; a = {k: o, a, b: unary()}; }
    if (peek('%') || peek('**')) fail(t[p].line, 'R5', `${t[p].k}`);
    return a;
  }
  function expr() {
    let a = term();
    while (peek('+') || peek('-')) { const o = t[p++].k; a = {k: o, a, b: term()}; }
    const k = t[p].k;
    if (['<', '>', '<=', '>=', '==', '===', '!=', '!==', '&&', '||', '?', '&', '|', '^'].includes(k)) {
      if (!allowCompare) fail(t[p].line, 'R5', `${k} in an expression`);
    }
    return a;
  }
  let allowCompare = false, loops = 0;
  const names = e => e.k === 'name' ? [e.v] : [e.a, e.b, ...(e.args ?? [])].filter(Boolean).flatMap(names);
  const assignedIn = body => body.flatMap(s => s.k === 'assign' ? [s.n] : s.k === 'decl' ? s.d.map(d => d.n) : s.body ? assignedIn(s.body) : []);
  const reads = body => body.flatMap(s => [
    ...(s.e ? names(s.e) : []), ...(s.d ? s.d.flatMap(d => names(d.e)) : []), ...(s.args ? s.args.flatMap(names) : []),
    ...(s.k === 'break' ? [...names(s.a), ...names(s.b)] : []), ...(s.n && typeof s.n === 'object' ? names(s.n) : []),
    ...(s.k === 'assign' && s.o !== '=' ? [s.n] : []), ...(s.body ? reads(s.body) : [])]);
  function block() {
    take('{'); scopes.push(new Map());
    const s = [];
    while (!peek('}')) { if (peek('eof')) fail(t[p].line, 'R1', 'unclosed block'); s.push(...stmt()); }
    take('}'); scopes.pop();
    return s;
  }
  function stmt() {
    const line = t[p].line, x = t[p];
    if (x.k === 'id' && KEYWORDS.has(x.v))
      fail(line, x.v === 'while' || x.v === 'do' || x.v === 'continue' ? 'R3' : x.v === 'else' ? 'R6' : 'R1', `${x.v}`);
    if (peek('let') || peek('const')) {
      const kind = t[p++].v, d = [];
      do {
        const n = take('id').v; take('=');
        d.push({n, e: expr()}); declare(n, kind, line);
      } while (peek(',') && take(','));
      take(';'); return [{k: 'decl', kind: 'let', d, line}];
    }
    if (peek('for')) return forLoop(line);
    if (peek('if')) {
      p++; take('('); allowCompare = true;
      const a = expr();
      if (!peek('>') && !peek('<')) fail(line, 'R6', `if (... ${t[p].v ?? t[p].k} ...)`);
      const o = t[p++].k, b = expr(); allowCompare = false; take(')');
      const braced = peek('{');
      if (braced) p++;
      take('break', 'R6'); take(';');
      if (braced) take('}', 'R6');
      if (peek('else')) fail(t[p].line, 'R6', 'else');
      if (!loops) fail(line, 'R6', 'break outside a for');
      return [{k: 'break', a: o === '>' ? a : b, b: o === '>' ? b : a, line}];
    }
    if (x.k !== 'id') fail(line, 'R1', `a statement starting with ${x.k}`);
    const n = t[p++].v;
    if (peek('(')) {
      if (!['move', 'line', 'plot', 'cubic'].includes(n)) fail(line, 'R5', `a call to ${n}()`);
      take('('); const args = [];
      if (!peek(')')) do args.push(expr()); while (peek(',') && take(','));
      take(')'); take(';');
      const want = {move: 2, line: 3, plot: 3, cubic: 10}[n];
      if (args.length !== want) fail(line, 'R1', `${n}() takes ${want} arguments`);
      return [{k: 'draw', f: n, args, line}];
    }
    const b = lookup(n);
    if (!b) fail(line, 'R4', `${n} is not declared here`);
    if (b.kind !== 'let') fail(line, 'R4', `${n} is ${b.kind === 'const' ? 'a const' : 'an ' + b.kind} and cannot be assigned`);
    const o = t[p].k;
    if (o === '++' || o === '--') fail(line, 'R1', `${n}${o} (write ${n} += 1)`);
    if (!['=', '+=', '-=', '*='].includes(o)) fail(line, 'R1', `${o}`);
    p++; const e = expr(); take(';');
    return [{k: 'assign', n, o, e, line}];
  }
  function forLoop(line) {
    p++; take('(', 'R3');
    if (!peek('let')) fail(line, 'R3', 'the loop variable must be declared with let');
    p++; const v = take('id', 'R3').v; take('=', 'R3');
    const zero = take('num', 'R3');
    if (+zero.v !== 0) fail(line, 'R3', `the loop must start at 0`);
    take(';', 'R3');
    if (take('id', 'R3').v !== v) fail(line, 'R3', 'the condition must test the loop variable');
    take('<', 'R3');
    scopes.push(new Map([[v, {kind: 'const'}]]));
    const n = expr(); take(';', 'R3');
    if (peek('++') && peek(v, 1)) p += 2;
    else if (peek(v) && peek('++', 1)) p += 2;
    else if (peek(v) && peek('+=', 1) && t[p + 2].k === 'num' && +t[p + 2].v === 1) p += 3;
    else fail(line, 'R3', 'the step must be i++, ++i or i += 1');
    take(')', 'R3');
    loops++; const body = block(); loops--;
    scopes.pop();
    if (names(n).includes(v)) fail(line, 'R3', 'the count reads the loop variable');
    const changed = new Set(assignedIn(body));
    for (const u of names(n)) if (changed.has(u)) fail(line, 'R3', `the count reads ${u}, which the body changes`);
    if (n.k === 'num' && +n.lit === 0) return [];
    if (!reads(body).includes(v)) return [{k: 'repeat', n, body, line}];
    // The counter as a variable: 0 before, += 1 after the body (a break skips
    // it, as JS's i++ is skipped when the loop is left).
    return [{k: 'decl', kind: 'let', d: [{n: v, e: {k: 'num', lit: '0'}}], line},
      {k: 'repeat', n, body: [...body, {k: 'assign', n: v, o: '+=', e: {k: 'num', lit: '1'}, line}], line}];
  }
  const body = [];
  while (!peek('eof')) body.push(...stmt());
  return body;
}

// Every /** ... @plan name ... inputs: a, b */ function f(args) { ... } in a
// file, and the method form f(args) { ... } (method: true), for a plan that
// is a property of an object literal.
export function findPlans(text, file = '') {
  const re = /\/\*\*((?:(?!\*\/)[\s\S])*?@plan\s[\s\S]*?)\*\/\s*(function\s+)?([A-Za-z_$][\w$]*)\s*\(([^)]*)\)\s*\{/g, out = [];
  let m;
  while ((m = re.exec(text))) {
    const doc = m[1], name = /@plan\s+([\w.$-]+)/.exec(doc)[1];
    const inputs = (/inputs:\s*([^\n*]*)/.exec(doc)?.[1] ?? '').split(',').map(s => s.trim()).filter(Boolean);
    if (inputs.length > 8) throw new PlanError(`${file}: ${name}: more than 8 inputs`);
    const params = m[4].split(',').map(s => s.trim()).filter(Boolean);
    const start = m.index + m[0].length, bodyLine = text.slice(0, start).split('\n').length;
    let depth = 1, i = start;
    for (; depth; ++i) {
      if (i >= text.length) throw new PlanError(`${file}: ${name}: unclosed function`);
      if (text.startsWith('//', i)) { i = text.indexOf('\n', i); continue; }
      if (text.startsWith('/*', i)) { i = text.indexOf('*/', i) + 1; continue; }
      if (text[i] === '{') depth++;
      if (text[i] === '}') depth--;
    }
    const fnStart = text.lastIndexOf('/**', m.index + 3);
    const head = m.index + m[0].length - m[0].replace(/^\/\*\*[\s\S]*?\*\/\s*/, '').length;
    out.push({name, fn: m[3], method: !m[2], inputs, params, body: text.slice(start, i - 1), line: bodyLine,
      start: fnStart, end: i, text: (m[2] ? '' : 'function ') + text.slice(head, i), file});
  }
  return out;
}
export function planAst(plan) {
  try {
    const body = parseBody(plan.body, plan.line, plan.inputs, plan.params);
    return [{k: 'input', d: plan.inputs.map((n, k) => ({n, k}))},
      ...plan.params.map((n, index) => ({k: 'param', n, index})), ...body];
  } catch (e) {
    if (e instanceof PlanError) e.message = `${plan.file}: ${plan.name}: ${e.message}`;
    throw e;
  }
}
export function compilePlan(plan) { return {...compileAst(planAst(plan)), plan}; }
export function compileFile(text, file) { return findPlans(text, file).map(compilePlan); }

// ------------------------------------------------------------ the reference
// Runs a plan's JS for one draw with move/line/plot/cubic recording segments,
// as ksn_procedural.c would: coordinates rounded half away from zero and
// bounded to -480..720, colours integral 0..65535, 1,024 segments and 8,192
// raster steps. f32: the function as the VM computes it, every + - * / and
// sin rounded to float32 (Math.fround), a non-finite result fails the draw;
// otherwise the function text runs as written, in double.
const F = Math.fround;
function recorder() {
  const seg = [];
  let pen = null, raster = 0, status = 'DONE';
  class Stop extends Error {}
  const stop = s => { status = s; throw new Stop(); };
  const coord = v => { if (!Number.isFinite(v) || v < -480 || v > 720) stop('INVALID'); return Math.sign(v) * Math.floor(Math.abs(v) + .5) | 0; };
  const colour = c => { if (!Number.isInteger(c) || c < 0 || c > 65535) stop('INVALID'); return c; };
  const emit = (x0, y0, x1, y1, c) => {
    if (seg.length === 1024) stop('LIMIT');
    const cost = Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0)) + 1;
    if (raster + cost > 8192) stop('LIMIT');
    raster += cost; seg.push([x0, y0, x1, y1, c]);
  };
  const api = {
    move: (x, y) => { pen = [coord(x), coord(y)]; },
    plot: (x, y, c) => { c = colour(c); const X = coord(x), Y = coord(y); emit(X, Y, X, Y, c); pen = [X, Y]; },
    line: (x, y, c) => { c = colour(c); const X = coord(x), Y = coord(y); if (pen) emit(pen[0], pen[1], X, Y, c); pen = [X, Y]; },
    cubic: (n, c, ...q) => {
      q = q.map(F); q.forEach(coord);
      let px = coord(q[0]), py = coord(q[1]);
      for (let s = 1; s <= n; ++s) {
        const t = F(F(s) / F(n)), u = F(1 - t);
        const b = (a0, a1, a2, a3) => F(F(F(F(F(F(u * u) * u) * a0) + F(F(F(F(3 * u) * u) * t) * a1)) +
          F(F(F(F(3 * u) * t) * t) * a2)) + F(F(F(t * t) * t) * a3));
        const x = coord(b(q[0], q[2], q[4], q[6])), y = coord(b(q[1], q[3], q[5], q[7]));
        emit(px, py, x, y, c); px = x; py = y;
      }
      pen = [px, py];
    },
  };
  return {api, Stop, result: () => ({status, seg, raster}), stopped: s => { status = s; }};
}
// kir AST -> JS with every operation rounded to float32.
function f32Js(ast, params) {
  const e = x => {
    switch (x.k) {
      case 'num': return `__F(${x.lit})`;
      case 'name': return x.v;
      case 'neg': return `-(${e(x.a)})`;
      case '+': case '-': case '*': case '/': return `__G(${e(x.a)} ${x.k} ${e(x.b)})`;
      case 'call': return `__G(__F(Math.sin(${e(x.args[0])})))`;
    }
    throw new Error(x.k);
  };
  // Colours, counts and CUBIC's segments are integers the VM reads from an
  // immediate or a register; as values they are floats like everything else.
  const s = list => list.map(x => {
    switch (x.k) {
      case 'input': return x.d.map(d => `const ${d.n} = __F(__in[${d.k}]);`).join('\n');
      case 'param': return `const ${x.n} = __F(__args[${x.index}]);`;
      case 'decl': return x.d.map(d => `let ${d.n} = ${e(d.e)};`).join('\n');
      case 'assign': return `${x.n} = ${x.o === '=' ? e(x.e) : `__G(${x.n} ${x.o[0]} ${e(x.e)})`};`;
      case 'break': return `if (${e(x.a)} > ${e(x.b)}) break;`;
      case 'draw': return `${x.f}(${x.args.map(e).join(', ')});`;
      case 'repeat': {
        const n = e(x.n), reg = !(x.n.k === 'num' || (x.n.k === 'name' && params.includes(x.n.v)));
        return `{ const __n = ${n}; if (!Number.isInteger(__n) || __n < ${reg ? 0 : 1} || __n > 255) __stop('INVALID');\n` +
          `for (let __k = 0; __k < __n; __k++) {\n${s(x.body)}\n} }`;
      }
    }
    throw new Error(x.k);
  }).join('\n');
  return s(ast);
}
const cache = new WeakMap();
export function reference(plan, input, args, {f32 = true} = {}) {
  const r = recorder();
  let fn = cache.get(plan)?.[f32];
  if (!fn) {
    const src = f32
      ? `return function (__in, __args) {\n${f32Js(planAst(plan), plan.params)}\n}`
      : `return function (__in, __args) {\nconst [${plan.inputs.join(', ')}] = __in;\nconst sin = Math.sin;\nreturn (${plan.text})(...__args);\n}`;
    fn = new Function('move', 'line', 'plot', 'cubic', '__F', '__G', '__stop', src);
    cache.set(plan, {...cache.get(plan), [f32]: fn});
  }
  const G = v => { v = F(v); if (!Number.isFinite(v)) r.stopped('INVALID'), (() => { throw new r.Stop(); })(); return v; };
  try {
    fn(r.api.move, r.api.line, r.api.plot, r.api.cubic, F, G, st => { r.stopped(st); throw new r.Stop(); })(input, args);
  } catch (e) { if (!(e instanceof r.Stop)) throw e; }
  return r.result();
}
