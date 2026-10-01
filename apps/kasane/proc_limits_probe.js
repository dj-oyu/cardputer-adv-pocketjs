// Diagnostic only (KASANE_PROC_LIMITS_PROBE). Why: docs/kasane/procedural-limits-device.md
(function () {
  'use strict';
  const SET = 0, INPUT = 1, ADD = 2, MUL = 3, SIN = 4, REP = 5, END = 6,
    MOVE = 7, PLOT = 8, LINE = 9, PCR = 12;
  const I = (o, d, a, b, v, c) => [o, d | 0, a | 0, b | 0, Math.fround(v || 0), c | 0];
  const rep = (n, f) => { for (let i = 0; i < n; i++) f(i); };
  function nest8() {
    const c = [];
    rep(8, i => c.push(I(INPUT, 8 + i, i)));
    c.push(I(ADD, 1, 10, 15), I(ADD, 2, 12, 15));
    rep(4, () => c.push(I(REP, 0, 2)));
    c.push(I(ADD, 0, 8, 15));
    rep(4, () => c.push(I(REP, 0, 2)));
    c.push(I(PCR, 2, 0, 1), I(ADD, 0, 0, 9), I(ADD, 2, 2, 13));
    rep(4, () => c.push(I(END)));
    c.push(I(ADD, 1, 1, 11), I(ADD, 2, 2, 14));
    rep(4, () => c.push(I(END)));
    return c;
  }
  const pts = (n, fx, fy, coeff, color) => ({ kind: 'affineQ14Points',
    x: Array.from({ length: n }, (_, i) => fx(i)),
    y: Array.from({ length: n }, (_, i) => fy(i)), coeff, color });
  const ID = [16384, 0, 0, 16384, 0, 0];
  const NOP = [I(SET, 0, 0, 0, 0)];
  const E8x = [-480, 720, -480, 720, -480, 720, 120, 120];
  const E8y = [-480, 720, 720, -480, 67, 67, -480, 720];
  const scene = { bg: 0x0841, draws: [
    { code: nest8(), inputs: [40, 5, 30, 4, 2016, 97, 301, 3] },
    { code: NOP, inputs: [], points: pts(128, i => i * 5 - 300,
      i => 67 + (i * 37) % 90 - 45, [15500, 3000, -2500, 16000, 40 * 16384, 10 * 16384], 0xffe0) },
    { code: NOP, inputs: [], points: pts(8, i => E8x[i], i => E8y[i], ID, 0xf81f) },
    { code: [I(SET, 0, 0, 0, -480), I(SET, 1, 0, 0, 700), I(SET, 2, 0, 0, 700),
      I(SET, 3, 0, 0, -480), I(MOVE, 0, 0, 1), I(LINE, 0, 2, 3, 0, 0x07ff)], inputs: [] }
  ] };
  globalThis.limScene = scene;
  if (typeof pocket === 'undefined' || typeof __lim === 'undefined') return;

  const L = __lim, P = pocket.kasane.procedural, V = pocket.kasane;
  const log = s => L.log(s);
  const code = f => { try { f(); return 'OK'; } catch (e) {
    const m = (e && e.code) || String(e);
    for (const k of ['INVALID_ARGUMENT', 'LIMIT_EXCEEDED', 'CLOSED', 'BUSY', 'OUT_OF_MEMORY'])
      if (String(m).includes(k)) return k;
    return String(m); } };
  let fails = 0;
  const expect = (name, f, want) => { const got = code(f);
    if (got !== want) fails++;
    log('CHECK ' + name + ' want=' + want + ' got=' + got); };
  const mem = tag => { const m = L.mem();
    log('MEM ' + tag + ' free=' + m[0] + ' largest=' + m[1] + ' stack_hwm=' + m[2] + ' min_free=' + m[3] + ' js=' + m[4]);
    return m; };
  const dots = [];
  rep(32, i => dots.push([I(SET, 0, 0, 0, 8 + (i * 13) % 224), I(SET, 1, 0, 0, 8 + (i * 7) % 120),
    I(PLOT, 0, 0, 1, 0, 0xffff - i)]));
  const b32 = pts(32, i => i * 7, i => 20 + (i * 11) % 90, ID, 0x07e0);
  const b128 = pts(128, i => i * 2 - 20, i => 60 + (i * 37) % 50 - 25, ID, 0xfd20);
  const sinLoop = [I(SET, 1, 0, 0, 1), I(SET, 2, 0, 0, 0.5), I(SET, 0), I(REP, 0, 212), I(REP, 0, 9),
    I(ADD, 0, 0, 1), I(SIN, 3, 0), I(ADD, 4, 3, 1), I(SIN, 5, 4), I(END), I(END)];
  const mulLoop = sinLoop.map(r => r[0] === SIN ? I(MUL, r[1], r[2], 2) : r);
  const res = P.resource();
  let regs = 0;
  const reg = (c, p) => { regs++; return p ? P.register(c, p) : P.register(c); };
  let st = null, last = 0;
  const stat = n => { st = { n: 0, name: n, sum: 0, max: 0, work: 0, wmax: 0, busy: 0, lost: 0, err: '' }; };
  const flush = () => log('FRAMES ' + st.name + ' n=' + st.n + ' avg_us=' + ((st.sum / st.n) | 0) +
    ' max_us=' + st.max + ' work_avg_us=' + ((st.work / st.n) | 0) + ' work_max_us=' + st.wmax +
    ' busy=' + st.busy + ' lost=' + st.lost + ' err=' + st.err + ' regs=' + regs);
  function draw(hs, bg) {
    try { P.beginFrame(bg || 0); } catch (e) { st.busy++; return false; }
    // A lost frame is counted, not fatal. Before the turn fix, any frame the
    // 8 ms budget parked was lost here with BUSY.
    try { for (const h of hs) P.draw(h, []); P.commit(); return true; }
    catch (e) { st.lost++; st.err = code(() => { throw e; }); return false; }
  }
  let live = [], ph = 0, t = 0, m0 = null, m1 = null, t0 = 0, oldH = 0, sinH = 0;
  const BK = [1, 2, 3, 4, 6, 8, 9];
  const churnLive = n => { const out = []; rep(n, i => out.push(reg(dots[(regs + i) % 32], (i & 1) ? b32 : undefined))); return out; };
  const freeAll = hs => { for (const h of hs) P.unregister(h); };
  const phases = [
    () => { if (t === 0) V.replace(tx => (tx.background(0x000000ff), tx.image({ resource: res, bounds: [0, 0, 240, 135],
      clip: [0, 0, 240, 135], sourceWidth: 240, sourceHeight: 135 })));
    if (t === 30) { L.gc(); m0 = mem('baseline'); live = churnLive(16); stat('steady16'); return true; }
    draw([]); },
    () => { draw(live); return t === 60; },
    () => { if (t === 0) { flush(); stat('churn1'); }
      P.unregister(live.shift()); live.push(reg(dots[regs % 32], (regs & 1) ? b32 : undefined));
      draw(live); return t === 150; },
    () => { if (t === 0) { flush(); stat('switch16'); }
      const next = churnLive(16); draw(next); freeAll(live); live = next; return t === 40; },
    () => { if (t === 0) { flush(); stat('steady16_after'); } draw(live); return t === 30; },
    () => { if (t === 0) flush(); freeAll(live); live = []; draw([]); return t === 5; },
    () => { L.gc(); m1 = mem('after_churn'); log('CHURN regs=' + regs + ' free_delta=' + (m1[0] - m0[0]) +
      ' largest_delta=' + (m1[1] - m0[1]) + ' js_delta=' + (m1[4] - m0[4]));
      live = churnLive(16); stat('switch16b'); return true; },
    () => { const next = churnLive(16); draw(next); freeAll(live); live = next; return t === 40; },
    () => { if (t === 0) flush(); freeAll(live); live = []; draw([]); return t === 5; },
    () => { L.gc(); m1 = mem('after_churn2'); log('CHURN2 regs=' + regs + ' free_delta_vs_baseline=' +
      (m1[0] - m0[0]) + ' largest=' + m1[1] + ' js_delta=' + (m1[4] - m0[4]));
      live = churnLive(16); stat('switch16c'); return true; },
    () => { const next = churnLive(16); draw(next); freeAll(live); live = next; return t === 40; },
    () => { if (t === 0) flush(); freeAll(live); live = []; draw([]); return t === 5; },
    () => { L.gc(); const m = mem('after_churn3'); log('CHURN3 regs=' + regs + ' free_delta_vs_baseline=' +
      (m[0] - m0[0]) + ' largest=' + m[1] + ' js_delta=' + (m[4] - m0[4]));
      draw([]); return true; },
    () => { // worst-case memory: 32 x 128 points. Native bytes are not charged to
      // the guest's heap limit, so free the plans before any other JS allocates.
      const hs = new Array(32).fill(0);
      L.gc(); const a = mem('before32'); let t1 = L.us(), n = 0, err = 'none';
      try { for (; n < 32; n++) hs[n] = reg(dots[n], b128); } catch (e) { err = e; }
      const us32 = L.us() - t1; const b = L.mem();
      for (let i = 0; i < n; i++) P.unregister(hs[i]);
      if (err !== 'none') err = code(() => { throw err; });
      log('WORST plans=' + n + ' points=128 bytes=' + (a[0] - b[0]) + ' free=' + b[0] + ' largest=' + b[1] +
        ' min_free=' + b[3] + ' js_delta=' + (b[4] - a[4]) + ' register_us=' + (us32 | 0) + ' err=' + err);
      return true; },
    () => { // concurrent limit and stale handles, with point-less plans
      rep(32, i => live.push(reg(dots[i])));
      expect('register33', () => reg(dots[0]), 'LIMIT_EXCEEDED');
      // register() allocates before it looks for a free slot, so at low heap the
      // 33rd batch reports OUT_OF_MEMORY first; logged, not counted as a fail.
      log('OBSERVE register33_points got=' + code(() => reg(dots[0], b128)));
      const old = oldH = live[7];
      expect('unregister_mid', () => P.unregister(old), 'OK');
      let h = 0; expect('reregister', () => { h = reg(dots[7]); }, 'OK');
      log('HANDLE reused_slot_handle=' + h + ' max_previous=' + Math.max(...live) + ' fresh=' + (h > Math.max(...live)));
      live[7] = h;
      expect('stale_unregister', () => P.unregister(old), 'CLOSED');
      freeAll(live); live = []; rep(9, i => live.push(reg(dots[i], b128)));
      return true; },
    () => { try { P.beginFrame(0); } catch (e) { return false; }
      // Checked in its own frame: registrations can outlast one turn.
      expect('stale_draw', () => P.draw(oldH, []), 'CLOSED');
      // 128 points make 127 segments: the 1,024-segment frame cap is reached first.
      let n = 0; try { for (const x of live) { P.draw(x, []); n++; } } catch (e) { log('FRAMELIMIT drawn=' + n + ' of=9 err=' + code(() => { throw e; })); }
      code(() => P.commit()); freeAll(live); live = [];
      return true; },
    () => { if (t < 3) return; // let the committed frame present first
      L.gc(); const a = mem('after_worst');
      log('WORST_RELEASE free_delta_vs_baseline=' + (a[0] - m0[0]) + ' largest_delta_vs_baseline=' + (a[1] - m0[1]));
      try { const sid = P.createSurface(); P.resource(sid); P.beginFrame(0, sid); }
      catch (e) { log('SURFACE2 err=' + code(() => { throw e; })); }
      const b = mem('surface2');
      log('SURFACE2 bytes=' + (a[0] - b[0]) + ' largest_drop=' + (a[1] - b[1]));
      let n = 0;
      try { rep(32, i => { live.push(reg(dots[i], b128)); n++; }); }
      catch (e) { log('SURFACE2_FILL err=' + code(() => { throw e; })); }
      const c = mem('surface2_plans');
      log('SURFACE2_FILL plans=' + n + ' bytes=' + (b[0] - c[0]) + ' largest_drop=' + (b[1] - c[1]));
      freeAll(live); live = []; L.gc(); const d = mem('surface2_released');
      log('SURFACE2_RELEASE free_delta=' + (d[0] - b[0]) + ' largest_delta=' + (d[1] - b[1]));
      return true; },
    () => { // negative checks for the new bounds
      expect('nest9', () => reg([...Array(9).fill(I(REP, 0, 2)), ...Array(9).fill(I(END))]), 'INVALID_ARGUMENT');
      expect('reg16', () => reg([I(SET, 16, 0, 0, 1)]), 'INVALID_ARGUMENT');
      expect('input8', () => reg([I(INPUT, 0, 8)]), 'INVALID_ARGUMENT');
      expect('points129', () => reg(NOP, pts(129, i => i, i => i, ID, 1)), 'INVALID_ARGUMENT');
      const hi = reg(NOP, pts(8, i => i ? 10 : 721, i => 10, ID, 1));
      const lo = reg(NOP, pts(8, i => 10, i => i ? 10 : -481, ID, 1));
      const h8 = reg(nest8());
      P.beginFrame(0);
      expect('draw_x721', () => P.draw(hi, []), 'INVALID_ARGUMENT');
      P.beginFrame(0);
      expect('draw_y-481', () => P.draw(lo, []), 'INVALID_ARGUMENT');
      P.beginFrame(0);
      expect('inputs9', () => P.draw(h8, [0, 0, 0, 0, 0, 0, 0, 0, 0]), 'INVALID_ARGUMENT');
      freeAll([hi, lo, h8]);
      L.gc(); mem('before_scene');
      live = scene.draws.map(d => d.points ? reg(d.code, d.points) : reg(d.code));
      stat('scene'); return true; },
    () => { const c0 = L.counts();
      P.beginFrame(scene.bg);
      const us = scene.draws.map((d, i) => { const t1 = L.us(); P.draw(live[i], d.inputs); return (L.us() - t1) | 0; });
      P.commit();
      if (t === 1) log('DRAWUS nest8=' + us[0] + ' points128=' + us[1] + ' points8=' + us[2] + ' vmline=' + us[3]);
      const c1 = L.counts();
      if (t === 0) log('SCENE hash=' + L.hash() + ' scalar=' + (c1[0] - c0[0]) + ' pie=' + (c1[1] - c0[1]));
      if (t === 2) log('LIMITS_SCENE_READY');
      return t === 300; },
    () => { flush(); freeAll(live); live = [];
      const h = reg(sinLoop); P.beginFrame(0); let t1 = L.us(); P.draw(h, []); const one = L.us() - t1;
      log('VMDRAW sin_one_us=' + (one | 0) + ' commit=' + code(() => P.commit())); P.unregister(h);
      rep(8, () => live.push(reg(mulLoop))); t0 = 0; return true; },
    () => { try { P.beginFrame(0); } catch (e) { return false; }
      t0++; const t1 = L.us(); let n = 0, err = 'none';
      try { for (const x of live.slice(0, t0)) { P.draw(x, []); n++; } } catch (e) { err = code(() => { throw e; }); }
      const us = L.us() - t1; code(() => P.commit());
      log('VMFRAME want=' + t0 + ' drawn=' + n + ' us=' + (us | 0) + ' err=' + err);
      return t0 >= 6; },
    () => { const h = reg(dots[0]); P.beginFrame(0); const t1 = L.us();
      while (L.us() - t1 < 12000);
      log('PARK draw_after_12ms=' + code(() => P.draw(h, [])) + ' commit=' + code(() => P.commit()));
      P.unregister(h); return true; },
    () => { freeAll(live); live = []; L.gc(); const m = mem('end');
      log('LIMITS_DONE fails=' + fails + ' regs=' + regs + ' free_delta=' + (m[0] - m0[0]) + ' largest_delta=' + (m[1] - m0[1]));
      return true; },
    () => { const b40 = pts(40, i => i * 5, i => 20 + (i * 11) % 90, ID, 0x07e0); let n = 0, err = 'none';
      try { for (; n < 32; n++) live.push(reg(NOP, b40)); } catch (e) { err = code(() => { throw e; }); }
      mem('p40'); log('P40 n=' + n + ' err=' + err); freeAll(live); live = []; L.gc(); return true; },
    () => { if (t === 0) sinH = reg(sinLoop);
      const k = BK[(t / 6) | 0];
      if (t % 6 === 0) { if (t) flush(); if (k === undefined) { log('BUDGET_DONE'); return true; } stat('budget' + k); }
      draw(Array(k).fill(sinH)); return false; },
    () => { if (t === 0) { stat('heavy4'); log('HEAVY_START'); } draw([sinH, sinH, sinH, sinH]); return false; }
  ];
  globalThis.frame = function (b) {
    // Back's save turn opens a frame first, as MEGADEMO's frame() does.
    if (b & 0x2000) { let m = 'OK'; try { P.beginFrame(0); } catch (e) { m = String(e); } log('LEAVE ph=' + ph + ' t=' + t + ' begin=' + m); }
    const now = L.us();
    if (st && last) { const dt = now - last; st.n++; st.sum += dt; if (dt > st.max) st.max = dt; }
    last = now;
    if (phases[ph]()) { ph++; t = 0; } else t++;
    if (st) { const w = L.us() - now; st.work += w; if (w > st.wmax) st.wmax = w; }
  };
  log('LIMITS_START');
})();
