(function () {
  const view = pocket.kasane.mount('hello');
  view.set({counter: 'NUMBENCH'});
  const T = () => pocket.time.now(), N = 40;
  const xs = [], ss = [];
  let seed = 12345;
  for (let i = 0; i < 16; i++) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    xs.push((seed / 2147483647) * Math.pow(10, (i % 9) - 4) + i / 7);
  }
  for (let i = 0; i < 16; i++) ss.push(String(xs[i]));
  const t = new Array(512).join('abcdefgh');
  const out = [];
  function bench(name, f, ops) {
    const t0 = T(); for (let k = 0; k < N; k++) f();
    out.push(name + ' ' + ((T() - t0) * 1000 / (N * ops)).toFixed(1) + 'us');
  }
  globalThis.frame = function () {
    if (out.length) return;
    let s = 0;
    bench('str', () => { for (let i = 0; i < 16; i++) s += String(xs[i]).length; }, 16);
    bench('parse', () => { for (let i = 0; i < 16; i++) s += parseFloat(ss[i]); }, 16);
    bench('num', () => { for (let i = 0; i < 16; i++) s += Number(ss[i]); }, 16);
    bench('toFixed', () => { for (let i = 0; i < 16; i++) s += xs[i].toFixed(3).length; }, 16);
    bench('upper4k', () => { s += t.toUpperCase().length; }, 1);
    bench('indexOf4k', () => { s += t.indexOf('z'); }, 1);
    bench('empty', () => { for (let i = 0; i < 16; i++) s += i; }, 16);
    console.log('NUMBENCH ' + out.join(' ') + ' ' + s);
  };
  console.log('HELLO_READY');
})();
