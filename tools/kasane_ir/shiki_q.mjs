// The Q-format rules of docs/kasane/shiki-spec.md sec.8.3, shared by the
// @shiki compiler (shiki_compile.mjs) and the reference that evaluates the
// same rules straight from the AST (q16_error.mjs's fix mode). Which Q a
// name has, which names are Q0, and what is a compile error are decided here
// once; the two then compute independently (one emits instructions for the
// engine, the other runs BigInt arithmetic), which is what their comparison
// tests.

// The largest Q (<= 30) at which a literal factor fits an int32 (rule 4).
export const litQ = v => { let q = 30; while (q > 0 && Math.abs(Math.round(v * 2 ** q)) > 2 ** 31 - 1) q--; return q; };

// round(2^32 / 2pi): radians at Q16 times TURN, shifted 32, is turns at Q16
// (rule 8).
export const TURN = 683565276;

// The integer arguments of the draw calls (rule 1): colours, counts, the
// pattern's bits and period.
export const INTS = {line: [2], plot: [2], cubic: [0, 1], linePattern: [2, 3, 4, 9]};

// The names a for...of over a data region unpacks, with the region's Q
// (rule 1, v1.3): they behave as @q names.
export function eachNames(ast, data = []) {
  const out = new Map();
  const walk = list => list.forEach(x => {
    // and a region.at(i)'s names (v2.0 draft LOAD), the same way.
    if (x.k === 'each' || x.k === 'load') { const d = data.find(r => r.name === x.r); for (const n of x.names) out.set(n, d.q); }
    if (x.body) walk(x.body);
  });
  walk(ast);
  return out;
}

// Q0 names (rule 1): counts, colours and integer draw arguments, the
// counters of for loops (plan_js: let v = 0, then the last statement of the
// repeat's body v += 1), take(n)'s count, from(k)'s start, the record of
// at(i) / set(i, ...), a name declared as drawIndex() (v2.0 draft), and every
// name joined to one of those by a plain name-to-name assignment. The names a for...of unpacks
// take no part in the joining (they are fixed at the region's Q).
export function q0Names(ast, fixed = new Set()) {
  const out = new Set(), links = [];
  const walk = list => list.forEach((x, k) => {
    if (x.k === 'each') { for (const e of [x.n, x.from]) if (e?.k === 'name') out.add(e.v); walk(x.body); }
    if ((x.k === 'load' || x.k === 'store') && x.i.k === 'name') out.add(x.i.v);
    if (x.k === 'decl') for (const d of x.d) if (d.e.k === 'call' && d.e.f === 'drawIndex') out.add(d.n);
    if (x.k === 'repeat') {
      if (x.n.k === 'name') out.add(x.n.v);
      const last = x.body[x.body.length - 1], prev = list[k - 1];
      if (last?.k === 'assign' && last.o === '+=' && last.e.k === 'num' && +last.e.lit === 1 &&
          prev?.k === 'decl' && prev.d.length === 1 && prev.d[0].n === last.n && prev.d[0].e.k === 'num' && +prev.d[0].e.lit === 0)
        out.add(last.n);
      walk(x.body);
    }
    if (x.k === 'draw') for (const i of INTS[x.f] ?? []) if (x.args[i]?.k === 'name') out.add(x.args[i].v);
    if (x.k === 'decl') for (const d of x.d) if (d.e.k === 'name') links.push([d.n, d.e.v]);
    if (x.k === 'assign' && x.o === '=' && x.e.k === 'name') links.push([x.n, x.e.v]);
  });
  walk(ast);
  for (let grew = true; grew;) {
    grew = false;
    for (const [a, b] of links) {
      if (fixed.has(a) || fixed.has(b)) continue;
      if (out.has(a) !== out.has(b)) { out.add(a); out.add(b); grew = true; }
    }
  }
  return out;
}

// Q(name) and natural(expr) for one plan, after the checks of rules 1 and 9
// that do not depend on the arithmetic: a declaration that contradicts Q0, a
// Q0 name given a value that can carry a fraction, a REPEAT literal outside
// 1..255. Each throws, as the compiler must.
export function qRules(ast, qdecl, data = []) {
  const each = eachNames(ast, data);
  for (const [n, q] of each) if (n in qdecl && qdecl[n] !== q) throw new Error(`@q ${n}=${qdecl[n]}: ${n} comes from a region at Q${q} (sec.8.3 rule 1)`);
  const ints = q0Names(ast, new Set(each.keys()));
  for (const [n, q] of Object.entries(qdecl)) if (ints.has(n) && q !== 0) throw new Error(`@q ${n}=${q}: ${n} is a count, colour or loop variable (Q0)`);
  const Q = n => each.has(n) ? each.get(n) : ints.has(n) ? 0 : qdecl[n] ?? 16;
  const regionQ = r => data.find(d => d.name === r).q;
  // A region's element read at the region's Q: a fraction unless that is 0.
  const fractional = e => e.k === 'num' ? !Number.isInteger(+e.lit) : e.k === 'name' ? Q(e.v) !== 0 :
    e.k === 'index' ? regionQ(e.r) !== 0 :
    e.k === 'call' ? e.f !== 'drawIndex' : e.k === '/' || (e.a && fractional(e.a)) || (e.b && fractional(e.b));
  const check = list => list.forEach(x => {
    if (x.k === 'decl') for (const d of x.d) if (ints.has(d.n) && fractional(d.e)) throw new Error(`${d.n} is Q0 (sec.8.3 rule 1) but is given a fraction`);
    if (x.k === 'assign' && ints.has(x.n) && fractional(x.e)) throw new Error(`${x.n} is Q0 (sec.8.3 rule 1) but is given a fraction`);
    if (x.k === 'repeat') {
      if (x.n.k === 'num' && !(Number.isInteger(+x.n.lit) && +x.n.lit >= 1 && +x.n.lit <= 255)) throw new Error(`REPEAT ${x.n.lit} outside 1..255`);
      check(x.body);
    }
    if (x.k === 'each') {
      // take(n): Q0, a whole count 0..65,535 when a literal (sec.8.2).
      if (x.n && fractional(x.n)) throw new Error(`${x.r}.take(): the count is Q0 (sec.8.3 rule 1) but can carry a fraction`);
      if (x.n?.k === 'num' && !(+x.n.lit >= 0 && +x.n.lit <= 65535)) throw new Error(`${x.r}.take(${x.n.lit}) outside 0..65,535`);
      if (x.from && fractional(x.from)) throw new Error(`${x.r}.from(): the start is Q0 (sec.8.3 rule 1) but can carry a fraction`);
      check(x.body);
    }
  });
  check(ast);
  // Rule 7: a name's declared Q, a literal follows the other side (null), any
  // other expression Q16.
  const natural = e => e.k === 'name' ? Q(e.v) : e.k === 'num' ? null : e.k === 'call' && e.f === 'drawIndex' ? 0 : 16;
  return {Q, natural, ints};
}

// `@q name=bits, ...` in each plan's doc comment, by plan name.
// Only doc comments (/** ... */) count: a line comment that mentions
// "@shiki" is prose.
export function qdecls(text) {
  const out = new Map(), re = /\/\*\*([^]*?)\*\//g;
  let m;
  while ((m = re.exec(text))) {
    const name = /@(?:plan|shiki)\s+([\w.$-]+)/.exec(m[1]);
    if (!name) continue;
    const q = /@q\s+([^\n*]*)/.exec(m[1]);
    out.set(name[1], Object.fromEntries((q?.[1] ?? '').split(',').map(t => t.trim().split('=')).filter(t => t[0]).map(([n, v]) => [n, Number(v)])));
  }
  return out;
}
