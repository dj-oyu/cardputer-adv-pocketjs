// LCD CATCH: fixed LCD segments, lit or ghosted on a beat. See README.md.
(function () {
  const V = pocket.kasane, K = pocket.input.keys;
  // Tuning (README.md). Times are frames at 30 fps; pairs are [GAME A, GAME B].
  const T = {
    tick: [13, 10], fast: 5, step: 20,
    spawn: [0.45, 0.6], ramp: 0.004, most: [0.8, 0.9],
    jump: [1, 2], missF: 36, lampF: 24, blinkF: 15,
    clear: [200, 500], gain: 0.3, click: 1600
  };
  const LCD = 0xb4bf9fff, GH = 0xa5b191ff, INK = 0x1e2619ff, PR = 0x8d9a7dff, GO = 26;
  const LX = [62, 104, 146, 188], SY = [22, 35, 48, 61];
  const DG = [0x3f, 6, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 7, 0x7f, 0x6f];
  const S7 = [[2, 0, 8, 2], [8, 2, 10, 7], [8, 9, 10, 15], [2, 15, 8, 17],
    [0, 9, 2, 15], [0, 2, 2, 7], [2, 7, 8, 9]];
  // Kasane keeps at most 32 live refs, so a star is one ref per row that moves
  // between that row's four fixed slots; the unlit slots are plain ghosts.
  const fx = [], ins = [], mv = [];
  const sky = new Uint8Array(16);
  const S = globalThis.lcdCatch = {mode: 0, fc: 0, wait: T.lampF, game: 0, pl: 1,
    score: 0, misses: 0, paused: false, splat: -1, last: 1, idle: 0, ticks: 0,
    hi: [0, 0], ops: 0, sky: sky};
  function cap(n) { const c = pocket.capabilities.get(n); return c.supported && c.available; }
  const au = cap('audio.tone') ? pocket.audio : null;
  const kv = cap('storage.kv') ? pocket.storage : null;
  const rng = pocket.random.create(pocket.random.seed());
  let saved = true;
  if (kv) kv.get('hi').then(function (r) {
    if (r && r.value) S.hi = [r.value[0] | 0, r.value[1] | 0];
  }, function () {});
  function beep(f, ms) {
    if (au) au.tone({frequencyHz: f, durationMs: ms, gain: T.gain}).catch(function () {});
  }
  function save() {
    if (!kv || saved) return;
    saved = true;
    kv.set('hi', S.hi).catch(function () { saved = false; });
  }
  const box = function (x, y) { return [x - 5, y, x + 6, y + 14]; };
  const man = V.cache.create([[0, 0, 16, 2], [3, 2, 13, 6], [13, 6, 15, 17], [5, 8, 11, 14],
    [4, 15, 12, 25]].map(function (b) { return {bounds: b, color: INK}; }));
  const pip = V.cache.create([{bounds: [0, 0, 7, 7], color: INK}]);

  // Everything starts lit: the power-on lamp test.
  V.replace(function (tx) {
    const X = function (b, t, f, c) {
      return tx.text({bounds: b, text: t, font: f || 'caption', color: c || INK});
    };
    tx.background(0x1b1f22ff);
    tx.roundRect({bounds: [4, 2, 236, 123], radius: 6, color: LCD});
    tx.rect({bounds: [8, 103, 232, 106], color: PR});
    tx.roundRect({bounds: [12, 27, 26, 41], radius: 7, color: PR});
    X([10, 126, 236, 134], 'A/D:MOVE 1:GAME A 2:GAME B TAB:PAUSE', 0, 0x7d858cff);
    for (let y = 0; y < 5; y++) {
      const b = [];
      for (let l = 0; l < 4; l++) {
        b.push(box(LX[l], y < 4 ? SY[y] : 107));
        X(b[l], y < 4 ? '+' : 'X', 'display', GH);
      }
      // setRect moves the bounds but not the clip, so the clip spans the row.
      const r = tx.text({bounds: b[y & 3], clip: [b[0][0], b[0][1], b[3][2], b[0][3]],
        text: y < 4 ? '+' : 'X', font: 'display', color: INK});
      mv.push({r: r, b: b, pos: y & 3, lit: 1});
    }
    fx.push(X([10, 6, 50, 14], 'GAME A'), X([10, 15, 50, 23], 'GAME B'),
      X([116, 6, 150, 14], 'PAUSE'), X([116, 15, 172, 23], 'GAME OVER'), X([178, 10, 192, 18], 'HI'));
    for (let d = 0; d < 3; d++) for (let k = 0; k < 7; k++) {
      const x = 196 + 13 * d, s = S7[k];
      fx.push(tx.rect({bounds: [x + s[0], 5 + s[1], x + s[2], 5 + s[3]], color: INK}));
    }
    for (let i = 0; i < 7; i++) {
      const o = i < 4 ? [LX[i] - 8, 77] : [66 + 11 * (i - 4), 10];
      ins.push({h: tx.instantiate(i < 4 ? man : pip, {offset: o}), o: o});
    }
  });
  const fw = new Uint8Array(fx.length), fc = new Uint8Array(fx.length).fill(1);
  const iw = new Uint8Array(ins.length), ic = new Uint8Array(ins.length).fill(1);
  const mw = new Int8Array(mv.length);

  function speed() { return Math.max(T.fast, T.tick[S.game] - (S.score / T.step | 0)); }
  function start(g) {
    sky.fill(0);
    Object.assign(S, {game: g, mode: 2, score: 0, misses: 0, splat: -1, idle: 0,
      last: S.pl, paused: false});
    S.wait = speed();
    beep(1000, 40);
  }
  function over() {
    S.mode = 4;
    if (S.score > S.hi[S.game]) { S.hi[S.game] = S.score; saved = false; save(); }
    console.log('LCDCATCH OVER ' + 'AB'[S.game] + ' ' + S.score + ' hi=' + S.hi[S.game]);
  }
  // One beat: resolve the star over the net, drop the rest one row, maybe add one.
  function tick() {
    let g = S.game;
    S.ticks++;
    for (let l = 0; l < 4; l++) if (sky[l * 4 + 3]) {
      sky[l * 4 + 3] = 0;
      if (l !== S.pl) {
        sky.fill(0);
        S.splat = l; S.misses++; S.mode = 3; S.wait = T.missF;
        beep(220, 350);
        return;
      }
      S.score = Math.min(999, S.score + 1);
      if (T.clear.indexOf(S.score) >= 0) S.misses = 0;
      beep(2100, 50);
      g = -1;
    }
    if (g >= 0 && T.click) beep(T.click, 12);
    g = S.game;
    for (let i = 15; i >= 0; i--) sky[i] = i & 3 ? sky[i - 1] : 0;
    if (rng.nextFloat() < Math.min(T.most[g], T.spawn[g] + S.score * T.ramp)) {
      const j = T.jump[g] + S.idle;
      let l;
      do l = rng.nextUint32() & 3; while (Math.abs(l - S.last) > j);
      sky[l * 4] = 1; S.last = l; S.idle = 0;
    } else S.idle++;
    S.wait = speed();
  }
  function step() {
    if (S.mode === 0) S.mode = 1;
    else if (S.mode === 2) tick();
    else if (S.mode === 3) {
      if (S.misses >= 3) over();
      else { S.splat = -1; S.mode = 2; S.wait = speed(); }
    }
    if (S.mode === 1 || S.mode === 4) S.wait = 1e9;
  }
  function paint() {
    const m = S.mode, t = m === 1;
    fw.fill(m ? 0 : 1); iw.fill(m ? 0 : 1);
    if (!m) { for (let y = 0; y < 5; y++) mw[y] = y & 3; return; }
    fw[S.game] = !t || (S.fc / T.blinkF | 0) % 2 === 0;
    fw[4] = t; fw[2] = S.paused; fw[3] = m === 4;
    const v = t ? S.hi[S.game] : S.score;
    for (let d = 0, p = 100; d < 3; d++, p /= 10) {
      const bits = v >= p || p === 1 ? DG[(v / p | 0) % 10] : 0;
      for (let k = 0; k < 7; k++) fw[5 + d * 7 + k] = bits >> k & 1;
    }
    iw[S.pl] = 1;
    for (let i = 0; i < S.misses; i++) iw[4 + i] = 1;
    mw.fill(-1);
    for (let i = 0; i < 16; i++) if (sky[i]) mw[i & 3] = i >> 2;
    if (S.splat >= 0 && (m !== 3 || S.fc >> 2 & 1)) mw[4] = S.splat;
  }
  // Only what changed is patched: a colour, a slot, or an instance's opacity.
  function flush() {
    paint();
    let ops = 0;
    for (let i = 0; i < fx.length; i++) ops += fw[i] !== fc[i];
    for (let i = 0; i < ins.length; i++) ops += iw[i] !== ic[i];
    for (let i = 0; i < mv.length; i++) {
      const l = mw[i], m = mv[i];
      ops += (l >= 0 && l !== m.pos) + (+(l >= 0) !== m.lit);
    }
    if (!ops) return;
    try {
      V.patch(function (tx) {
        for (let i = 0; i < fx.length; i++) if (fw[i] !== fc[i]) fx[i].setColor(tx, fw[i] ? INK : GH);
        for (let i = 0; i < ins.length; i++) if (iw[i] !== ic[i])
          ins[i].h.place(tx, {offset: ins[i].o, opacity: iw[i] ? 255 : GO});
        for (let i = 0; i < mv.length; i++) {
          const l = mw[i], m = mv[i];
          if (l >= 0 && l !== m.pos) { m.r.setRect(tx, m.b[l]); m.pos = l; }
          if (+(l >= 0) !== m.lit) { m.lit = +(l >= 0); m.r.setColor(tx, m.lit ? INK : GH); }
        }
      });
      fc.set(fw); ic.set(iw);
      S.ops = ops;
    } catch (e) {
      // Not applied: forget what is shown so the next frame rewrites it all.
      fc.fill(2); ic.fill(2);
      for (const m of mv) { m.pos = -1; m.lit = 2; }
    }
  }

  globalThis.frame = function (b) {
    S.fc++;
    if (b & 0x2000) {
      if (S.mode > 1 && S.mode < 4 && S.score > S.hi[S.game]) { S.hi[S.game] = S.score; saved = false; }
      save();
      return;
    }
    const m = S.mode;
    if (m && !S.paused) {
      if ((K.pressed('a') || K.pressed(',')) && S.pl > 0) S.pl--;
      if ((K.pressed('d') || K.pressed('/')) && S.pl < 3) S.pl++;
    }
    if (m === 1 || m === 4) {
      if (K.pressed('s') || K.pressed('.')) S.game ^= 1;
      if (K.pressed('1')) start(0);
      else if (K.pressed('2')) start(1);
      else if (K.pressed('e') || K.pressed(';')) start(S.game);
    } else if (m > 1 && K.pressed('tab')) S.paused = !S.paused;
    if (!S.paused && --S.wait <= 0) step();
    flush();
  };
})();
