// docs/kasane/shiki-spec.md's integer semantics, evaluated straight from a
// plan's AST as @shiki would compile it (sec.2.2, 2.4, 4.4, 4.5, 8.3): the
// reference the compiler (shiki_compile.mjs) is checked against
// (test_shiki_compile.mjs), and q16_error.mjs's fix mode. Every value is a
// BigInt raw int32 at the Q its consumer asks for; the Q rules themselves
// come from shiki_q.mjs.
import {INTS, litQ, qRules} from './shiki_q.mjs';

export class Fail extends Error { constructor(s) { super(s); this.status = s; } }
const MAX = 2n ** 31n - 1n, MIN = -(2n ** 31n);

// ------------------------------------------------ shiki-spec.md arithmetic
export const fix = {sat: 0, satIn: 0};
const sat = (v, src = 'sat') => {          // v: BigInt
  if (v > MAX) { fix[src]++; return MAX; }
  if (v < MIN) { fix[src]++; return MIN; }
  return v;
};
const floorDiv = (v, s) => v >= 0n ? v >> s : -((-v + (1n << s) - 1n) >> s); // floor(v / 2^s)
// sec.2.3: v * 2^q, clamped to the int32 range in double, then rounded half
// away from zero.
const lround = (v, q) => {
  if (!Number.isFinite(v)) throw new Fail('INVALID');
  const x = Math.min(Math.max(v * 2 ** q, -(2 ** 31)), 2 ** 31 - 1);
  return sat(BigInt(Math.sign(x) * Math.floor(Math.abs(x) + .5)), 'satIn');
};
const SIN = Array.from({length: 257}, (_, k) => Math.round(Math.sin(k * Math.PI / 512) * 65536));
const TURN = 683565276n;                    // round(2^32 / 2pi): radians Q16 * TURN >> 32 = turns Q16
const X = {
  add: (a, b) => sat(a + b), sub: (a, b) => sat(a - b),
  mul: (a, b, s) => s >= 0 ? sat(floorDiv(a * b, BigInt(s))) : sat(a * b * (1n << BigInt(-s))),
  shift: (a, k) => k >= 0 ? sat(a << BigInt(k)) : floorDiv(a, BigInt(-k)),
  input: (v, q) => lround(v, q),
  sin: a => {                                // sec.4.4, a in Q16 turns
    const p = Number(BigInt.asUintN(16, a)), quad = p >> 14;
    let w = p & 0x3fff;
    if (quad & 1) w = 0x4000 - w;
    let v;
    if (w === 0x4000) v = SIN[256];
    else { const i = w >> 6, t = w & 63; v = SIN[i] + Math.floor((SIN[i + 1] - SIN[i]) * t / 64); }
    return BigInt(quad >= 2 ? -v : v);
  },
};
// The plan's AST as JS on BigInt raw values, every node computed at the Q
// its consumer asks for (sec.8.3).
export function emitFix(ast, params, qdecl) {
  const {Q, natural} = qRules(ast, qdecl);
  const consts = [];
  const C = v => { consts.push(v); return `__c[${consts.length - 1}]`; };
  // sec.8.3 rule 9: what sec.6.1 would refuse is a compile error, not a
  // silent saturation.
  const lit = (v, q) => { const r = Math.round(Math.abs(v) * 2 ** q); if (r > 2 ** 31 - 1) throw new Error(`literal ${v} does not fit Q${q}`); return lround(v, q); };
  const S = s => { if (s > 62) throw new Error(`MUL shift ${s} over 62`); if (s < -31) throw new Error(`MUL shift ${s} under -31`); return s; };
  const K = k => { if (Math.abs(k) > 31) throw new Error(`SHIFT ${k} outside -31..31`); return k; };
  // A factor of *: [code, its Q].
  // A negated number is a number too (rule 4): (-1.2) * s.
  const factor = e => {
    if (e.k === 'neg' && e.a.k === 'num') e = {k: 'num', lit: String(-e.a.lit)};
    if (e.k === 'name') return [e.v, Q(e.v)];
    if (e.k === 'num') { const q = litQ(+e.lit); return [C(lit(+e.lit, q)), q]; }
    return [g(e, 16), 16];
  };
  const g = (e, q) => {
    switch (e.k) {
      case 'num': return C(lit(+e.lit, q));
      case 'name': { const d = K(q - Q(e.v)); return d ? `$.shift(${e.v}, ${d})` : e.v; }
      case 'neg': return `$.sub(0n, ${g(e.a, q)})`;
      case '+': return `$.add(${g(e.a, q)}, ${g(e.b, q)})`;
      case '-': return `$.sub(${g(e.a, q)}, ${g(e.b, q)})`;
      case '*': { const [a, qa] = factor(e.a), [b, qb] = factor(e.b); return `$.mul(${a}, ${b}, ${S(qa + qb - q)})`; }
      case '/': {
        const c = +e.b.lit, k = Math.log2(Math.abs(c));
        if (Number.isInteger(k) && k >= 0) {
          let body;
          if (e.a.k === '*') { const [a, qa] = factor(e.a.a), [b, qb] = factor(e.a.b); body = `$.mul(${a}, ${b}, ${S(qa + qb - q + k)})`; }
          else body = `$.shift(${g(e.a, q)}, ${K(-k)})`;
          return c < 0 ? `$.sub(0n, ${body})` : body;
        }
        return g({k: '*', a: e.a, b: {k: 'num', lit: String(1 / c)}}, q);
      }
      case 'call': {
        const s = `$.sin($.mul(${g(e.args[0], 16)}, ${TURN}n, 32))`;
        return q === 16 ? s : `$.shift(${s}, ${K(q - 16)})`;
      }
    }
    throw new Error(e.k);
  };
  const s = list => list.map(x => {
    switch (x.k) {
      case 'input': return x.d.map(d => `const ${d.n} = $.input(__in[${d.k}] ?? 0, ${Q(d.n)});`).join('\n');
      case 'param': return `const ${x.n} = $.input(__args[${x.index}], ${Q(x.n)});`;
      case 'decl': return x.d.map(d => `let ${d.n} = ${g(d.e, Q(d.n))};`).join('\n');
      case 'assign': return `${x.n} = ${g(x.o === '=' ? x.e : {k: x.o[0], a: {k: 'name', v: x.n}, b: x.e}, Q(x.n))};`;
      case 'break': {
        const qa = natural(x.a), qb = natural(x.b), q = Math.max(qa ?? qb ?? 16, qb ?? qa ?? 16);
        return `if (${g(x.a, q)} > ${g(x.b, q)}) break;`;
      }
      case 'draw': {
        const ii = INTS[x.f] ?? [];
        return `__draw.${x.f}(${x.args.map((a, i) => ii.includes(i) ? `$.int(${g(a, 0)})` : `$.real(${g(a, 16)})`).join(', ')});`;
      }
      case 'repeat': {
        const reg = !(x.n.k === 'num' || (x.n.k === 'name' && params.includes(x.n.v)));
        return `{ const __n = Number(${g(x.n, 0)}); if (__n < ${reg ? 0 : 1} || __n > 255) throw new __Fail('INVALID');\n` +
          `for (let __k = 0; __k < __n; __k++) {\n${s(x.body)}\n} }`;
      }
    }
    throw new Error(x.k);
  }).join('\n');
  const fn = new Function('$', '__c', '__draw', '__Fail', '__in', '__args', s(ast));
  return (m, draw, Fl, input, args) => fn(m, consts, draw, Fl, input, args);
}
export const FIXM = {...X, real: v => Number(v) / 65536, int: v => Number(v)};

