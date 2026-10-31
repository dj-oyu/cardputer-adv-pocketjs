// Integer instruction latency probe (README.md, docs/perf/int-latency.md).
'use strict';
const D = pocket.shikiDebug;
let i = 0, done = false;
console.log('CB BEGIN');
globalThis.frame = function () {
  if (done) return;
  const line = D.bench(i++);
  if (line === null) { console.log('CB END ' + (i - 1)); done = true; return; }
  console.log(line);
};
