// Stress test, vaporwave surf at dusk: heap churn + a full-screen Shiki frame. See README.md.
(function () {
  const V = pocket.kasane, P = V.shiki, fs = pocket.fs, CAP = [15, 35, 1e9];
  const rgb = v => (v >> 19 & 31) << 11 | (v >> 10 & 63) << 5 | (v >> 3 & 31);
  const PINK = 0xff71ceff, CYAN = 0x01cdfeff, MINT = 0x05ffa1ff, SUN = 0xfffb96ff, INK = 0x1a0033ff;
  const FULL = [0, 0, 240, 135], BG = rgb(INK >>> 8);
  let t = 0, lvl = 0, pool = [], peak = 0, oom = 0, suspect = 0, cyc = 0, err = 0;
  let pressure = 0, pressureEvents = 0, pressureTrims = 0;
  // Allocation-free; LV3 sheds the pool only on a real failure (README).
  const memory = pocket.memory;
  if (memory) memory.onPressure(function (mask) {
    pressure = mask;
    pressureEvents++;
    if (lvl === 2 && (mask & memory.FAILURE)) { pool.length = 0; pressureTrims++; }
  });
  let nat = 0, natB = 0, natOk = '?', h = null, busy = false, fresh = false;
  let meter, stat, info, lv;

  // Shiki regions (window index = field * S + record, S = records rounded up to 8).
  // The palette: an 80s surf poster at dusk, neon over a lo-fi CRT.
  const mix = (a, b, f) => { let o = 0; for (let s = 0; s < 24; s += 8) o |= Math.round((a >> s & 255) * (1 - f) + (b >> s & 255) * f) << s; return o; };
  const grad = (st, f) => { let i = 0; while (i < st.length - 2 && f > st[i + 2]) i += 2; return mix(st[i + 1], st[i + 3], (f - st[i]) / (st[i + 2] - st[i])); };
  const win = (type, rows, k) => [P.data({type: type, q: 0, records: rows.length, record: k}), rows.length + 7 & ~7];
  const fill = (w, rows) => { for (let j = 0; j < rows.length; j++) for (let c = 0; c < rows[j].length; c++) w[0][c * w[1] + j] = rows[j][c]; };
  const SKY = [0, 0x14052e, .45, 0x5a1a8c, .8, 0xff3cac, 1, 0xff9a3d], SEA = [0, 0x19c6d8, .35, 0x0a6f9c, 1, 0x07103a];
  const ramp = P.data({type: 'u16', q: 0, records: 135, record: 1});
  for (let y = 0; y < 135; y++)   // every other row a little darker: scanlines
    ramp[y] = rgb(y === 60 ? 0xfff1b0 : mix(0, y < 60 ? grad(SKY, y / 59) : grad(SEA, (y - 61) / 73), y & 1 ? .8 : 1));
  const tw = P.data({type: 'u16', q: 0, records: 8, record: 1});
  for (let i = 0; i < 8; i++) tw[i] = rgb(mix(0x3a1466, 0xfff6ff, i / 7));
  const ST = [];
  for (let i = 0; i < 24; i++) ST.push([i * 53 % 236 + 2, i * 29 % 34 + 2, i * 17 % 62]);
  const stars = win('i16', ST, 3); fill(stars, ST);
  const SR = [];
  for (let y = 16; y < 60; y++)   // the disc of radius 26 about (176, 42), cut by the horizon
    SR.push([y, Math.sqrt(676 - (y - 42) * (y - 42)) | 0, rgb(grad([0, 0xfff07a, .5, 0xff9a3d, 1, 0xff2fa0], (y - 16) / 43))]);
  const sun = win('u16', SR, 3); fill(sun, SR);
  const wc = P.data({type: 'u16', q: 0, records: 12, record: 1});
  for (let r = 0; r < 12; r++) wc[r] = rgb(grad([0, 0xffb3e6, .4, 0xff5fd2, 1, 0x3ff4ff], r / 11));
  const FR = [[-12, -14, -34, 6], [-10, -20, -26, -10], [-2, -22, -8, -22], [8, -22, 12, -18],
    [12, -16, 30, -6], [14, -8, 34, 12], [4, -2, 16, 24]];
  const fronds = win('i16', FR, 4); fill(fronds, FR);
  // The rim's polyline: each leaf's curve (crown's first) at 1/3 and 2/3, and its tip.
  const RF = FR.map(f => { const r = [];
    for (let n = 0; n < 4; n++) { const i = n & 1, u = n < 2 ? 12 : 6;
      r.push(Math.round((u * f[i] + (18 - u) * (f[i + 2] - 3 - 2 * i) + (n < 2 ? 1 : 8) * f[i + 2]) / 27)); }
    return r.concat(f[2], f[3]); });
  const rims = win('i16', RF, 6); fill(rims, RF);
  const lt = P.data({type: 'u16', q: 0, records: 8, record: 1});   // the rim light, dark to sunlit
  for (let i = 0; i < 8; i++) lt[i] = rgb(grad([0, 0x2a0638, .45, 0xc2187f, .75, 0xff7a3d, 1, 0xfff3a0], i / 7));
  const pal = [0xff3cac, 0xb6ff3a, 0x19e6d4, 0xffe14d].map(rgb);
  const SC = [];
  for (let i = 0; i < 9; i++) SC.push([i * 29 % 72, i * 13 % 30, pal[i & 3]]);
  const school = win('u16', SC, 3); fill(school, SC);
  const D = [[0, 1], [5, 6], [16, 4], [19, 0], [22, 4], [34, 5], [41, 7], [34, 10], [20, 12], [5, 9], [0, 14], [2, 7], [0, 1]];
  const shape = win('i16', D, 2); fill(shape, D);
  const RW = P.data({type: 'i16', q: 0, records: 8, record: 3}), SW = sun[0], BY = new Float64Array(8);
  for (let i = 0; i < 8; i++) { BY[i] = 64 + i * 37 % 42; RW[16 + i] = 2 + i % 4; }

  const reg = (n, a, d) => P.register('stress.' + n, a, d ? {data: d} : undefined);
  const hSky = reg('sky', [], [ramp]), hStar = reg('stars', [], [stars[0], tw]), hSun = reg('sun', [], [sun[0]]);
  const hGlow = reg('glow', [rgb(0xfff07a), rgb(0xfff07a) + rgb(0xff5fd2)]), hWave = reg('waves', [], [wc]);
  const hFish = reg('fish', [], [school[0]]), hDol = reg('dolphin', [rgb(0x3ff4ff), rgb(0xfff6ff)], [shape[0]]);
  const hDol2 = reg('dolphin', [rgb(0xff5fd2), rgb(0xffe14d)], [shape[0]]);
  const hBub = reg('bubbles', [rgb(0xc8fbff)], [RW]), hTrunk = reg('trunk', [rgb(0x16031f), 8]), hCrown = reg('crown', [rgb(0x16031f), 5], [fronds[0]]);
  const hRim = reg('rim', [], [rims[0], lt]), hVhs = reg('vhs', [rgb(0xfff6ff), rgb(0xff3cac)]);
  // One drawMany a frame, five inputs a draw (Q16, sky's n Q0), back to front:
  // about 900 of the frame's 1,024 segments. IN[d * 5 + k]:
  const DR = [[hSky, 0, 34], [hSky, 68, 34], [hStar], [hSun], [hGlow], [hWave], [hFish], [hFish], [hDol], [hDol2],
    [hBub], [hTrunk, 12, 136, 1], [hCrown, 12, 136, 1], [hRim, 12, 136, 1], [hTrunk, 230, 140, -1],
    [hCrown, 230, 140, -1], [hRim, 230, 140, -1], [hVhs]];
  const STAR = 2, GLOW = 4, WAVE = 5, FISH = 6, DOL = 8, PALM = 11, VHS = 17, U = 65536;
  const H = new Uint32Array(18), IN = new Int32Array(90);
  for (let i = 0; i < 18; i++) { const d = DR[i]; H[i] = d[0]; for (let k = 1; k < d.length; k++) IN[i * 5 + k - 1] = d[k] * U; }
  IN[1] = IN[6] = 34;
  const res = P.resource();
  let shLv, shNat, shOk;

  function scene(tx) {
    tx.background(INK);   // a replace is refused without one
    tx.image({resource: res, bounds: FULL, clip: FULL, sourceWidth: 240, sourceHeight: 135});
    tx.text({bounds: [7, 4, 190, 17], text: 'S T R E S S   S E A', font: 'body', color: PINK});
    tx.text({bounds: [6, 3, 190, 16], text: 'S T R E S S   S E A', font: 'body', color: 0xffffffff, opacity: 200});
    lv = tx.text({bounds: [204, 3, 238, 16], text: '', capacity: 4, font: 'body', color: MINT});
    tx.roundRect({bounds: [2, 110, 238, 134], radius: 4, color: INK, opacity: 190});
    tx.strokeRect({bounds: [2, 110, 238, 134], width: 1, color: CYAN});
    meter = tx.gradient({bounds: [4, 111, 5, 113], axis: 'x', from: MINT, to: PINK});
    stat = tx.text({bounds: [6, 113, 236, 123], text: '', capacity: 48, font: 'caption', color: SUN});
    info = tx.text({bounds: [6, 123, 236, 133], text: '', capacity: 48, font: 'caption', color: CYAN});
    shLv = shNat = -1;
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
    // An OOM whose message failed to allocate: the host checks it (README).
    else if (lvl === 2 && memory && (memory.pressure() & memory.GUEST) && e &&
             e.name === 'InternalError' && !e.message) {
      suspect++; console.log('STRESS_SUSPECT n=' + suspect + ' at=' + where);
    }
    else { err++; console.log('STRESS_FAIL ' + where + ' ' + e); }
  }

  // Inputs as Q16, rounded as drawMany rounds a Float64Array (README), the
  // moving rows into their windows, one Shiki frame. Phases within 2 pi.
  const TAU = Math.PI * 2;
  let sk = -1;
  function paint() {
    IN[STAR * 5] = t * .07 % TAU * U + .5 | 0; IN[GLOW * 5] = t * .05 % TAU * U + .5 | 0;
    IN[WAVE * 5] = t * .11 % TAU * U + .5 | 0;
    if (t >> 2 !== sk) {   // the sun's gaps climb, wider lower down
      sk = t >> 2;
      for (let y = 35; y < 60; y++) SW[y - 16] = (y + sk) % 7 < (y - 30) / 8 ? 300 : y;
    }
    for (let k = 0; k < 2; k++) {
      const f = (FISH + k) * 5, d = (DOL + k) * 5, m = (PALM + 3 * k) * 5;
      IN[f] = (t * (1 + k) + k * 150) % 340 - 100 << 16;
      IN[f + 1] = (66 + k * 18 + 5 * Math.sin((t + k * 20) / 14)) * U + .5 | 0;
      const p = (t * 2 + k * 170) % 340, a = Math.sin(p / 340 * TAU);
      IN[d] = p - 50 << 16;
      IN[d + 1] = (a > 0 ? 88 - 58 * a : 88 - 8 * a) * U + .5 | 0;   // leap above the horizon, then glide
      const w = 2.5 * Math.sin(t / (23 - 4 * k) + k) * U;   // the palms: trunk, crown and rim sway as one
      IN[m + 3] = IN[m + 8] = IN[m + 13] = w + (w < 0 ? -.5 : .5) | 0;
      IN[m + 14] = t * (.06 + .02 * k) % TAU * U + .5 | 0;
    }
    for (let k = 0; k < 8; k++) {
      let y = BY[k] - (0.3 + k % 3 * 0.25);
      if (y < 64) y = 104;
      BY[k] = y; RW[k] = 8 + k * 29 + 3 * Math.sin((t + k * 9) / 10); RW[8 + k] = y;
    }
    const g = t % 150;   // a VHS glitch every 5 s, not drawn between
    IN[VHS * 5] = 18 + g * 7 << 16; IN[VHS * 5 + 1] = t * .9 % TAU * U + .5 | 0;
    P.beginFrame(BG);
    P.drawMany(H, IN, 5, g < 14 ? 18 : 17);
    P.commit();
  }

  function hud(tx) {
    meter.setRect(tx, 4, 111, 4 + Math.min(232, pool.length * 232 / 60 | 0), 113);
    if (lvl !== shLv) { lv.setText(tx, 'LV' + (lvl + 1)); shLv = lvl; }
    stat.setText(tx, 'pool ' + pool.length + ' peak ' + peak + ' oom ' + oom + ' f' + t);
    if (nat !== shNat || natOk !== shOk) { info.setText(tx, 'native ' + natOk + ' ' + nat + ' reads ' + (natB >> 10) + 'K'); shNat = nat; shOk = natOk; }
  }
  function rebuild(tx) { scene(tx); hud(tx); }

  V.replace(rebuild);
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
      if (fresh || t % 120 === 0) { fresh = true; V.replace(rebuild); fresh = false; } else V.patch(hud);
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
