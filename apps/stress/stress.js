// Stress test, vaporwave sea: heap churn + a full-screen Shiki frame. See README.md.
(function () {
  const V = pocket.kasane, P = V.shiki, fs = pocket.fs, CAP = [15, 35, 1e9];
  const rgb = v => (v >> 19 & 31) << 11 | (v >> 10 & 63) << 5 | (v >> 3 & 31);
  const PINK = 0xff71ceff, CYAN = 0x01cdfeff, MINT = 0x05ffa1ff, SUN = 0xfffb96ff, INK = 0x1a0033ff;
  const FULL = [0, 0, 240, 135], BG = rgb(INK >>> 8);
  let t = 0, lvl = 0, pool = [], peak = 0, oom = 0, suspect = 0, cyc = 0, err = 0;
  let pressure = 0, pressureEvents = 0, pressureTrims = 0;
  // The host owns pressure detection. Keep the callback allocation-free: LV3
  // sheds retained load only after a real allocation failure, preserving the
  // stress test's deliberate first OOM and its recovery check.
  const memory = pocket.memory;
  if (memory) memory.onPressure(function (mask) {
    pressure = mask;
    pressureEvents++;
    if (lvl === 2 && (mask & memory.FAILURE)) { pool.length = 0; pressureTrims++; }
  });
  let nat = 0, natB = 0, natOk = '?', h = null, busy = false, fresh = false;
  let meter, stat, info, lv;

  // Shiki regions (window index = field * S + record, S = records rounded up to 8).
  const mix = (a, b, f) => { let o = 0; for (let s = 0; s < 24; s += 8) o |= Math.round((a >> s & 255) * (1 - f) + (b >> s & 255) * f) << s; return o; };
  const ramp = P.data({type: 'u16', q: 0, records: 135, record: 1});
  for (let y = 0; y < 135; y++)
    ramp[y] = rgb(y < 60 ? mix(0x2a0a5e, PINK >>> 8, y / 59) : y === 60 ? MINT >>> 8 : mix(CYAN >>> 8, INK >>> 8, (y - 61) / 73));
  const SR = [];
  for (let k = 0; k < 6; k++)   // striped sun: bars of a disc of radius 26 about y 42
    for (let y = 16 + k * 7, e = y + 5 - (k >> 1); y < e; y++)
      SR.push([y, Math.sqrt(676 - (y - 42) * (y - 42)) | 0, rgb(0xff0000 | (0xfb - k * 22) << 8 | (0x96 - k * 6))]);
  const sun = P.data({type: 'u16', q: 0, records: SR.length, record: 3}), ss = SR.length + 7 & ~7;
  SR.forEach((r, j) => { sun[j] = r[0]; sun[ss + j] = r[1]; sun[2 * ss + j] = r[2]; });
  const pal = [CYAN, PINK, MINT, SUN].map(c => rgb(c >>> 8));
  const school = P.data({type: 'u16', q: 0, records: 9, record: 3});
  for (let i = 0; i < 9; i++) { school[i] = i * 29 % 72; school[16 + i] = i * 13 % 30; school[32 + i] = pal[i & 3]; }
  const D = [0, 1, 5, 6, 16, 4, 19, 0, 22, 4, 34, 5, 41, 7, 34, 10, 20, 12, 5, 9, 0, 14, 2, 7, 0, 1];
  const shape = P.data({type: 'i16', q: 0, records: D.length / 2, record: 2}), ds = D.length / 2 + 7 & ~7;
  for (let j = 0; j < D.length / 2; j++) { shape[j] = D[2 * j]; shape[ds + j] = D[2 * j + 1]; }
  const ring = P.data({type: 'i16', q: 0, records: 8, record: 3});
  const B = [];
  for (let i = 0; i < 8; i++) B.push({x: 8 + i * 29, y: 64 + i * 37 % 42, r: 2 + i % 4, s: 0.3 + i % 3 * 0.25});

  const reg = (n, a, d) => P.register('stress.' + n, a, d ? {data: [d]} : undefined);
  const hSky = reg('sky', [], ramp), hSun = reg('sun', [], sun), hGrid = reg('grid', [pal[1]]);
  const hFish = reg('fish', [], school), hDol = reg('dolphin', [rgb(0x9fb8ff), rgb(0xeef4ff)], shape);
  const hBub = reg('bubbles', [rgb(0xe8feff)], ring);
  // One drawMany a frame, two inputs a draw: 4 sky bands, sun, grid, 2 schools, 2 dolphins, bubbles.
  const H = new Uint32Array([hSky, hSky, hSky, hSky, hSun, hGrid, hFish, hFish, hDol, hDol, hBub]);
  const IN = new Float64Array(H.length * 2);
  IN.set([0, 34, 34, 34, 68, 34, 102, 33]);
  const res = P.resource();
  const txt = () => ['pool ' + pool.length + ' peak ' + peak + ' oom ' + oom + ' f' + t,
    'native ' + natOk + ' ' + nat + ' reads ' + (natB >> 10) + 'K', 'LV' + (lvl + 1)];

  function scene(tx) {
    tx.background(INK);   // a replace is refused without one
    tx.image({resource: res, bounds: FULL, clip: FULL, sourceWidth: 240, sourceHeight: 135});
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

  function readRejected() { busy = false; }

  async function readChunk() {
    busy = true;
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
    // At the quota, QuickJS can reject allocating the OOM error's message.
    // Keep this separate from confirmed OOM: the host must correlate it with
    // the VM canary from this turn before counting the trial as clean.
    else if (lvl === 2 && memory && (memory.pressure() & memory.GUEST) && e &&
             e.name === 'InternalError' && !e.message) {
      suspect++; console.log('STRESS_SUSPECT n=' + suspect + ' at=' + where);
    }
    else { err++; console.log('STRESS_FAIL ' + where + ' ' + e); }
  }

  // The sea: positions into IN and the bubble ring, then one Shiki frame.
  function paint() {
    IN[10] = t / 2 % 9.6;
    for (let k = 0; k < 2; k++) {
      IN[12 + 2 * k] = (t * (1 + k) + k * 150) % 340 - 100;
      IN[13 + 2 * k] = 64 + k * 16 + 5 * Math.sin((t + k * 20) / 14);
      const p = (t * 2 + k * 170) % 340, a = Math.sin(p / 340 * Math.PI * 2);
      IN[16 + 2 * k] = p - 50;
      IN[17 + 2 * k] = a > 0 ? 86 - 58 * a : 86 - 8 * a;   // leap above the horizon, then glide
    }
    B.forEach(function (b, k) {
      b.y -= b.s; if (b.y < 62) b.y = 104;
      ring[k] = b.x + 3 * Math.sin((t + k * 9) / 10); ring[8 + k] = b.y; ring[16 + k] = b.r;
    });
    P.beginFrame(BG);
    P.drawMany(H, IN, 2);
    P.commit();
  }

  function hud(tx) {
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
      if (!busy && (t & 1)) {
        try { readChunk().catch(readRejected); }
        catch (e) { busy = false; throw e; }
      }
      paint();
      // A replace that failed part-way (an OOM) left every ref stale: rebuild.
      if (fresh || t % 120 === 0) { fresh = true; V.replace(tx => { scene(tx); hud(tx); }); fresh = false; } else V.patch(hud);
      if (t % 60 === 0) {
        const s = V.stats(), sh = P.heap();
        console.log('STRESS f=' + t + ' lvl=' + (lvl + 1) + ' pool=' + pool.length + ' peak=' + peak +
          ' oom=' + oom + ' cyc=' + cyc + ' nat=' + nat + ' natOk=' + natOk + ' err=' + err +
          ' cmds=' + s.displayed.commands + ' native=' + s.nativeBytes +
          ' pressure=' + pressure + ' pe=' + pressureEvents + ' trim=' + pressureTrims +
          ' shiki=' + sh.used + '/' + sh.bytes);
      }
    } catch (e) { fresh = true; fail('frame', e); }
  };
  console.log('STRESS_READY');
})();
