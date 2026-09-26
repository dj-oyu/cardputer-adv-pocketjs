// Stress test, vaporwave sea: heap churn + many alpha shapes. See README.md.
(function () {
  const V = pocket.kasane, fs = pocket.fs, CAP = [15, 35, 1e9];
  const SEA = [0, 61, 240, 110];   // a moved shape keeps its clip, which defaults to where it began
  const PINK = 0xff71ceff, CYAN = 0x01cdfeff, MINT = 0x05ffa1ff, SUN = 0xfffb96ff, INK = 0x1a0033ff;
  let t = 0, lvl = 0, pool = [], peak = 0, oom = 0, cyc = 0, err = 0;
  let nat = 0, natB = 0, natOk = '?', h = null, busy = false, fresh = false;
  let sch = [], dol = [], bub = [], grid = [], meter, stat, info, lv;

  const f = [], pal = [CYAN, PINK, MINT, SUN];
  for (let i = 0; i < 9; i++) {   // an instance spends its commands (+1) out of the scene's 80
    const x = i * 29 % 72, y = i * 13 % 30, c = pal[i & 3];
    f.push({bounds: [x + 3, y + 1, x + 12, y + 5], color: c},
      {bounds: [x, y, x + 3, y + 6], color: c, opacity: 150});
  }
  const school = V.cache.create(f);
  const dolphin = V.cache.create([
    {bounds: [4, 4, 34, 11], color: 0x9fb8ffff}, {bounds: [9, 9, 30, 12], color: 0xeef4ffff},
    {bounds: [34, 6, 41, 9], color: 0x9fb8ffff}, {bounds: [16, 0, 22, 4], color: 0x7f8fe8ff},
    {bounds: [0, 1, 4, 14], color: 0x7f8fe8ff}]);
  const B = [];
  for (let i = 0; i < 8; i++) B.push({x: 8 + i * 29, y: 64 + i * 37 % 42, r: 2 + i % 4, s: 0.3 + i % 3 * 0.25});
  const txt = () => ['pool ' + pool.length + ' peak ' + peak + ' oom ' + oom + ' f' + t,
    'native ' + natOk + ' ' + nat + ' reads ' + (natB >> 10) + 'K', 'LV' + (lvl + 1)];

  function scene(tx) {
    tx.background(INK);   // a replace is refused without one
    tx.gradient({bounds: [0, 0, 240, 60], axis: 'y', from: 0x2a0a5eff, to: PINK, dither: true});
    for (let k = 0; k < 6; k++) {   // striped sun: radius is capped at 8, so bars
      const y = 16 + k * 7, d = 26 - (y + 2 - 16), w = Math.sqrt(676 - d * d) | 0;
      tx.rect({bounds: [176 - w, y, 176 + w, y + 5 - (k >> 1)],
        color: (0xff << 24 | (0xfb - k * 22) << 16 | (0x96 - k * 6) << 8 | 0xff) >>> 0});
    }
    tx.gradient({bounds: [0, 60, 240, 135], axis: 'y', from: CYAN, to: INK, dither: true});
    tx.rect({bounds: [0, 60, 240, 61], color: MINT});
    grid = [];
    for (let k = 0; k < 4; k++) grid.push(tx.rect({bounds: [0, 62, 240, 63], clip: SEA, color: PINK, opacity: 150}));
    sch = [];
    for (let k = 0; k < 2; k++) sch.push(tx.instantiate(school, {offset: [0, 64], opacity: 170 + k * 60}));
    dol = [tx.instantiate(dolphin, {offset: [-50, 80]}), tx.instantiate(dolphin, {offset: [-50, 80]})];
    bub = B.map(b => tx.roundRect({bounds: [b.x, b.y, b.x + 2 * b.r, b.y + 2 * b.r], radius: b.r,
      clip: SEA, color: 0xe8feffff, opacity: 150}));
    const s = txt();
    tx.text({bounds: [7, 4, 190, 17], text: 'S T R E S S   S E A', font: 'body', color: PINK});
    tx.text({bounds: [6, 3, 190, 16], text: 'S T R E S S   S E A', font: 'body', color: 0xffffffff, opacity: 200});
    lv = tx.text({bounds: [204, 3, 238, 16], text: s[2], capacity: 4, font: 'body', color: MINT});
    tx.roundRect({bounds: [2, 110, 238, 134], radius: 4, color: INK, opacity: 190});
    tx.strokeRect({bounds: [2, 110, 238, 134], width: 1, color: CYAN});
    meter = tx.gradient({bounds: [4, 111, 5, 113], axis: 'x', from: MINT, to: PINK});
    stat = tx.text({bounds: [6, 113, 236, 123], text: s[0], capacity: 48, font: 'caption', color: SUN});
    info = tx.text({bounds: [6, 123, 236, 133], text: s[1], capacity: 48, font: 'caption', color: CYAN});
  }

  function chunk(k) {
    switch (k % 5) {
      case 0: { const a = []; for (let i = 0; i < 24; i++) a.push({i: i, s: 'n' + i + k, v: [i, k]}); return a; }
      case 1: return 's' + k + 'x'.repeat(200 + k % 300);
      case 2: return new Float32Array(128);
      case 3: return new DataView(new ArrayBuffer(256 + k % 256));
      default: { const a = {k: k}, b = {a: a}; a.b = b; cyc++; return new Int16Array(64); }
    }
  }

  function load() {
    for (let j = 0; j < 2; j++) pool.push(chunk(t * 2 + j));
    if (pool.length > peak) peak = pool.length;
    if (pool.length > CAP[lvl]) pool.splice(0, pool.length >> 1);
  }

  async function readChunk() {
    try {
      if (!h) h = await fs.open('assets:/hello.js', {mode: 'read'});
      const c = await h.read(1024);
      if (!c) { h.close(); h = null; return; }
      if (natOk === '?') {
        const p = Object.getPrototypeOf(c), k = p.constructor;
        natOk = (k === Uint8Array && p === Uint8Array.prototype) ? 'ok' : 'NG';
        console.log('STRESS_NATIVE ' + natOk + ' len=' + c.length);
      }
      nat++; natB += c.length;
      pool.push(c);
    } catch (e) {
      fail('read', e);
      if (h) try { h.close(); } catch (x) {}
      h = null;
    } finally { busy = false; }
  }

  // An OOM is the point of L3: drop the load FIRST, then report.
  function fail(where, e) {
    pool.length = 0;
    if (e === null || /memory/.test(e)) { oom++; console.log('STRESS_OOM n=' + oom + ' at=' + where); }
    else { err++; console.log('STRESS_FAIL ' + where + ' ' + e); }
  }

  function draw(tx) {
    for (let k = 0; k < 4; k++) {
      const q = (t / 2 + k * 12) % 48, y = 62 + q * q / 50 | 0;   // perspective: faster near us
      grid[k].setRect(tx, [0, y, 240, y + 1]);
    }
    for (let k = 0; k < 2; k++) {
      const x = (t * (1 + k) + k * 150) % 340 - 100, y = 64 + k * 16 + 5 * Math.sin((t + k * 20) / 14);
      sch[k].place(tx, {offset: [x | 0, y | 0], opacity: 170 + k * 60});
    }
    for (let k = 0; k < 2; k++) {
      const p = (t * 2 + k * 170) % 340, x = p - 50, a = Math.sin(p / 340 * Math.PI * 2);
      const y = a > 0 ? 86 - 58 * a : 86 - 8 * a;   // leap above the horizon, then glide
      dol[k].place(tx, {offset: [x, y | 0]});
    }
    B.forEach(function (b, k) {
      b.y -= b.s; if (b.y < 62) b.y = 104;
      const x = b.x + 3 * Math.sin((t + k * 9) / 10) | 0, y = b.y | 0;
      bub[k].setRect(tx, [x, y, x + 2 * b.r, y + 2 * b.r]);
    });
    const s = txt();
    meter.setRect(tx, [4, 111, 4 + Math.min(232, pool.length * 232 / 60 | 0), 113]);
    lv.setText(tx, s[2]); stat.setText(tx, s[0]); info.setText(tx, s[1]);
  }

  V.replace(scene);
  globalThis.frame = function (buttons) {
    t++;
    try {
      if (buttons & 0x4000) { lvl = (lvl + 1) % 3; peak = 0; console.log('STRESS_LEVEL ' + (lvl + 1)); }
      load();
      if (!busy && (t & 1)) { busy = true; readChunk(); }
      // A replace that failed part-way (an OOM) left every ref stale: rebuild.
      // draw() in the same replace: scene() alone shows the start positions for a frame.
      if (fresh || t % 120 === 0) { fresh = true; V.replace(tx => { scene(tx); draw(tx); }); fresh = false; } else V.patch(draw);
      if (t % 60 === 0) {
        const s = V.stats();
        console.log('STRESS f=' + t + ' lvl=' + (lvl + 1) + ' pool=' + pool.length + ' peak=' + peak +
          ' oom=' + oom + ' cyc=' + cyc + ' nat=' + nat + ' natOk=' + natOk + ' err=' + err +
          ' cmds=' + s.displayed.commands + ' native=' + s.nativeBytes);
      }
    } catch (e) { fresh = true; fail('frame', e); }
  };
  console.log('STRESS_READY');
})();
