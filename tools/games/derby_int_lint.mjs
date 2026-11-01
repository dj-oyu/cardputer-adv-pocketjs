// DERBY WATCH's JS without doubles (docs/apps/derby-integer.md sec.4): lists
// what would make one outside the @plan / @shiki bodies (compiled, not run as
// JS) and comments: '/', a literal with a fraction or exponent, a Math
// function that returns a fraction, and a Float64Array.
//
//   node tools/games/derby_int_lint.mjs [--strict] [apps/derby]
//
// Prints one line per hit and a count per file. --strict exits 1 on any hit
// (the gate once the integer stages are in).
import fs from 'node:fs';
import path from 'node:path';
import {findPlans} from '../kasane_ir/plan_js.mjs';

const args = process.argv.slice(2), strict = args.includes('--strict');
const dir = args.find(a => !a.startsWith('--')) ?? 'apps/derby';
const RULES = [
  ['div', /(?<![/*])\/(?![/*=])/g],
  ['fraction', /(?<![\w$.])(?:\d+\.\d*|\.\d+|\d+(?:\.\d*)?[eE][+-]?\d+)(?![\w$])/g],
  ['math', /\b(?:M|Math)\.(?:sqrt|pow|exp|log|sin|cos|tan|atan2?|hypot|cbrt|PI|E|random|fround)\b/g],
  ['float64', /\bFloat(?:64|32)Array\b/g],
];
// Blanks comments, strings and template text (keeping line breaks), so the
// rules see code only.
function code(text) {
  let out = '', i = 0;
  const blank = s => s.replace(/[^\n]/g, ' ');
  while (i < text.length) {
    const c = text[i], n = text[i + 1];
    if (c === '/' && n === '/') { const e = text.indexOf('\n', i); const j = e < 0 ? text.length : e; out += blank(text.slice(i, j)); i = j; }
    else if (c === '/' && n === '*') { const j = text.indexOf('*/', i + 2) + 2; out += blank(text.slice(i, j)); i = j; }
    else if (c === '"' || c === "'" || c === '`') {
      let j = i + 1;
      while (j < text.length && text[j] !== c) j += text[j] === '\\' ? 2 : 1;
      out += c + blank(text.slice(i + 1, j)) + c; i = j + 1;
    } else { out += c; ++i; }
  }
  return out;
}
let total = 0;
for (const f of fs.readdirSync(dir).filter(f => f.endsWith('.js')).sort()) {
  const file = path.join(dir, f), text = fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n');
  let src = code(text);
  // Compiled bodies are not JS at run time.
  for (const p of findPlans(text, file, ['plan', 'shiki']))
    src = src.slice(0, p.start) + src.slice(p.start, p.end).replace(/[^\n]/g, ' ') + src.slice(p.end);
  const lines = src.split('\n');
  let n = 0;
  lines.forEach((line, k) => {
    for (const [name, re] of RULES)
      for (const m of line.matchAll(re)) {
        ++n;
        if (!args.includes('--count')) console.log(`${file}:${k + 1}: ${name}: ${m[0]}`);
      }
  });
  console.log(`${file}: ${n}`);
  total += n;
}
console.log(`total ${total}`);
if (strict && total) process.exit(1);
