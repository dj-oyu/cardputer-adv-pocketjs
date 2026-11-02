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
  // Data regions (sec.5.4, v1.3). store: PUT's saturation to the element
  // type (0 i16, 1 u16, 2 i32); at: LOOKUP's index floor(v / 2^g) clamped
  // to 0..n-1.
  store: (t, v) => Number(t === 0 ? (v > 32767n ? 32767n : v < -32768n ? -32768n : v)
    : t === 1 ? (v > 65535n ? 65535n : v < 0n ? 0n : v) : v),
  at: (v, g, n) => { const i = floorDiv(v, BigInt(g)); return i < 0n ? 0 : i > BigInt(n - 1) ? n - 1 : Number(i); },
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
// its consumer asks for (sec.8.3). data: the plan's @data regions; the
// function then takes their memory (typed arrays, region order) and reads
// and writes it as sec.5.4 says.
export function emitFix(ast, params, qdecl, data = []) {
  const {Q, natural} = qRules(ast, qdecl, data);
  const reg = r => { const i = data.findIndex(d => d.name === r); if (i < 0) throw new Error(`${r} is not a region`); return [i, data[i]]; };
  const loops = [];   // the for...of loops around the current statement: {r, cursor}
  let uid = 0;
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
        // drawIndex() (v2.0 draft DRAW_INDEX): the draw's number, Q0.
        if (e.f === 'drawIndex') { const d = K(q); return d ? `$.shift(BigInt(__idx), ${d})` : 'BigInt(__idx)'; }
        const s = `$.sin($.mul(${g(e.args[0], 16)}, ${TURN}n, 32))`;
        return q === 16 ? s : `$.shift(${s}, ${K(q - 16)})`;
      }
      case 'index': {
        // LOOKUP: the index at its own Q (a name's, else Q16) is g.
        const [i, d] = reg(e.r);
        if (d.k !== 1) throw new Error(`${e.r}[...]: LOOKUP needs a region of record 1`);
        const gq = e.i.k === 'name' ? Q(e.i.v) : 16;
        return `$.shift(BigInt(__data[${i}][$.at(${g(e.i, gq)}, ${gq}, __data[${i}].length)]), ${K(q - d.q)})`;
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
      case 'each': {
        // EACH (+ FETCH): the start (v2.0 draft, 0..records) then N (0..records
        // - start), each read once, or INVALID; each name is its element at
        // the region's Q.
        const [i, d] = reg(x.r), c = `__cur${uid++}`;
        if (x.names.length > d.k) throw new Error(`${x.names.length} names for a record of ${d.k}`);
        const rec = `(__data[${i}].length / ${d.k})`;
        const st = x.from ? `Number(${g(x.from, 0)})` : '0';
        const n = x.n ? `Number(${g(x.n, 0)})` : `${rec} - __s`;
        loops.push({r: x.r, c});
        const body = s(x.body);
        loops.pop();
        return `{ const __s = ${st}; if (__s < 0 || __s > ${rec}) throw new __Fail('INVALID');\n` +
          `const __n = ${n}; if (__n < 0 || __n > ${rec} - __s) throw new __Fail('INVALID');\n` +
          `for (let ${c} = __s; ${c} < __s + __n; ${c}++) {\n` +
          // v1.4 field planes (as shiki_ref.mjs keeps them, no padding): field
          // j of record c at j * R + c.
          x.names.map((v, j) => `const ${v} = BigInt(__data[${i}][${j} * ${rec} + ${c}]);`).join('\n') + `\n${body}\n} }`;
      }
      case 'put': {
        const [i, d] = reg(x.r), l = [...loops].reverse().find(o => o.r === x.r);
        if (!l) throw new Error(`${x.r}.put() outside a for...of over ${x.r}`);
        if (!d.write) throw new Error(`${x.r}.put(): the region is not declared write`);
        if (x.args.length < 1 || x.args.length > d.k) throw new Error(`${x.r}.put() takes 1..${d.k} values`);
        // Every value first, then the writes (sec.5.4.4).
        return `{ const __v = [${x.args.map(a => g(a, d.q)).join(', ')}];\n` +
          x.args.map((_, j) => `__data[${i}][${j} * (__data[${i}].length / ${d.k}) + ${l.c}] = $.store(${d.type}, __v[${j}]);`).join('\n') + ' }';
      }
      case 'load': case 'store': {
        // v2.0 draft LOAD / STORE: record i (Q0) in 0..records - 1 or
        // INVALID; fields c.. of it. STORE: every value first, then the writes.
        const [i, d] = reg(x.r), u = uid++, rec = `(__data[${i}].length / ${d.k})`;
        const m = x.k === 'load' ? x.names.length : x.args.length;
        if (m < 1 || x.c + m > d.k) throw new Error(`${x.r}.${x.k === 'load' ? 'at' : 'set'}(): fields ${x.c}..${x.c + m - 1} of a record of ${d.k}`);
        if (x.k === 'store' && !d.write) throw new Error(`${x.r}.set(): the region is not declared write`);
        const head = `const __r${u} = Number(${g(x.i, 0)}); if (__r${u} < 0 || __r${u} >= ${rec}) throw new __Fail('INVALID');\n`;
        if (x.k === 'load')
          return head + x.names.map((v, j) => `const ${v} = BigInt(__data[${i}][${x.c + j} * ${rec} + __r${u}]);`).join('\n');
        return `{ ${head}const __v = [${x.args.map(a => g(a, d.q)).join(', ')}];\n` +
          x.args.map((_, j) => `__data[${i}][${x.c + j} * ${rec} + __r${u}] = $.store(${d.type}, __v[${j}]);`).join('\n') + ' }';
      }
      case 'repeat': {
        const reg = !(x.n.k === 'num' || (x.n.k === 'name' && params.includes(x.n.v)));
        return `{ const __n = Number(${g(x.n, 0)}); if (__n < ${reg ? 0 : 1} || __n > 255) throw new __Fail('INVALID');\n` +
          `for (let __k = 0; __k < __n; __k++) {\n${s(x.body)}\n} }`;
      }
    }
    throw new Error(x.k);
  }).join('\n');
  const fn = new Function('$', '__c', '__draw', '__Fail', '__in', '__args', '__data', '__idx', s(ast));
  return (m, draw, Fl, input, args, regions = [], index = 0) => fn(m, consts, draw, Fl, input, args, regions, index);
}
export const FIXM = {...X, real: v => Number(v) / 65536, int: v => Number(v)};

