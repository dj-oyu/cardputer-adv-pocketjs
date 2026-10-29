// MEGADEMO chunk 5: the view's state, the plan loader and the set's build
// and patch (megademo_view.js has its tables). patch() writes a ref only
// when its value changes: every write is a view command to re-check.
'use strict';
// The full screen, the monitor in the room, and the camera's steps between.
const FULL = [0, 0, 240, 135], MON = [64, 20, 176, 83], ZOOM = 44;
// scene starts at WAKE, so the loop's first enter(7) is not a new night.
// live: the running scene's plan handles (PN plans at tier LT; the loader
// adds the rest a frame at a time); next: the next scene's, prefetched.
let tier = KN.length - 1, scene = 7, t = 0, tick = 0, live = [], PN = 0, LT = 0, next = null, loaded = 0, nd = 0;
let img, mon, hud = [], hw = [], R = {}, built = -1, vis = false, said = -1, zb = -1, ZB = FULL;
let zoom = ZOOM, target = ZOOM, spd = 2, pin = false, fixed = false, held = 0;
const drop = h => { for (let i = 0; i < h.length; ++i) H.unregister(h[i]); };
const reg = (s, i) => { const p = plan(plist(s, tier)[i]); ++loaded; return p[1] ? H.register(p[0], p[1]) : H.register(p[0]); };
const lerp4 = (a, b, e) => a.map((v, j) => rnd(v + (b[j] - v) * e)), clamp = v => v < 0 ? 0 : v > 1 ? 1 : v;
function prefetch(s, max) {
  if (s < 3 && scene < 3 || s === 3 && scene === 2) return;
  if (next && (next.s !== s || next.k !== tier)) { drop(next.h); next = null; }
  if (!next) next = {s: s, k: tier, h: [], n: plist(s, tier).length};
  while (max-- > 0 && next.h.length < next.n && MEM.info().internalFreeBytes >= LOAD[2])
    next.h.push(reg(s, next.h.length));
}
// fresh: drop the prefetched set too (tier change, recovery). The old set
// goes before the rest registers: they overlap only as far as the
// memory gate let the prefetch run.
function enter(s, f) {
  // Act I's scenes share one plan set.
  if (s < 3 && scene < 3 && PN) {
    scene = s; t = 0;
    return console.log('MEGADEMO SCENE ' + NAMES[s] + ' tier=' + tier + ' plans=' + live.length + ' freed=0 registered=' + loaded);
  }
  if (f && next) { drop(next.h); next = null; }
  const o = live.length;
  if (s === 3 && scene === 2 && LT === tier && !f) {
    // TWIST takes over its plans from Act I's set; the HUD's go.
    if (next) { drop(next.h); next = null; }
    const h = live.splice(6);
    drop(live); live = h; PN = plist(3, tier).length;
  } else {
    prefetch(s, 0);
    if (s === 7 && scene !== 7) ++sortie;
    drop(live); live = next.h; PN = next.n; LT = next.k; next = null;
  }
  scene = s; t = 0;
  console.log('MEGADEMO SCENE ' + NAMES[s] + ' tier=' + tier + ' plans=' + live.length +
    ' freed=' + o + ' registered=' + loaded);
}
// Enter is dead while the story owns the camera: on waking (first person,
// the TV is off), CALL (the TV turns itself on, the dive into it), and
// from SIGNAL CRITICAL in CORE through the cut-out (LOST), which must play
// on the full screen straight into WAKE's dark.
const locked = () => scene === 7 || scene === 0 || scene === 6 || scene === 5 && t >= 84;
// WAKE's eyelids (0 shut .. 1 open): open halfway, a blink, open; shut
// over the cut to the room (WCUT) and open again, sitting up.
const lid = t => t < 10 ? 0 : t < 18 ? .55 * smooth((t - 10) / 8) : t < 21 ? .55 - (t - 18) / 6 :
  t < 30 ? .05 + .95 * smooth((t - 21) / 9) : t < 69 ? 1 : t < 72 ? (72 - t) / 3 : t < 75 ? 0 : t < 80 ? (t - 75) / 5 : 1;
const boundsAt = step => step === 0 ? FULL : step === ZOOM ? MON :
  lerp4(FULL, MON, step / ZOOM * step / ZOOM * (3 - 2 * step / ZOOM));
// Build helpers (bt: the build's transaction). A HUD ref shows on the full
// screen while from <= t < to.
let bt = null;
const add = (r, from, to) => { hud.push(r); hw.push(from || 0, to || 1e9, 1); return r; };
const text = (b, s, f, c, n) => bt.text({bounds: b, text: s, font: f, color: c, capacity: n});
// "x0 y0 x1 y1 rrggbbaa radius|-stroke|opacity" (a text when 5 has letters).
function box(s) {
  const e = s.split(' '), b = [+e[0], +e[1], +e[2], +e[3]], c = parseInt(e[4], 16), r = +e[5] || 0;
  return e[5] > 'A' ? text(b, e[5], 'caption', c) :
    r > 0 ? bt.roundRect({bounds: b, radius: r, color: c}) : r < 0 ?
      bt.strokeRect({bounds: b, width: -r, color: c}) : bt.rect({bounds: b, color: c, opacity: +e[6] || 255});
}
function build(tx, m) {
  bt = tx; hud = []; hw = []; R = {}; vis = false; said = -1; zb = zoom; ZB = boundsAt(zoom);
  const ch = CHAP[scene], S = R.S = SAY[scene] && SAY[scene].split('|'), T = TITLE.split('|');
  tx.background(255);
  if (m) {
    tx.gradient({bounds: [0, 0, 240, 100], axis: 'y', from: 0x0a0618ff, to: GLOW[ch], dither: true});
    ROOM.split(',').forEach(box);
    // A tally on the wall, a mark a night: four strokes and a fifth across
    // them (four steps, corner to corner), three gates a row. Each rect is
    // a view command (80 a scene; the room's set is 43 without the tally
    // in ZENITH), so the wall holds four gates (20 nights; 75 commands);
    // the SORTIE line keeps counting.
    for (let i = 0; i < M.min(20, night()); ++i) {
      const g = (i / 5) | 0, k = i % 5, x = 197 + (g % 3) * 14, y = 16 + ((g / 3) | 0) * 14;
      if (k < 4) tx.rect({bounds: [x + 3 * k, y, x + 3 * k + 1, y + 10], color: 0x8a7c66ff});
      else for (let s = 0; s < 4; ++s)
        tx.rect({bounds: [x - 1 + 3 * s, y + 8 - 2 * s, x + 2 + 3 * s, y + 10 - 2 * s], color: 0x8a7c66ff});
    }
    tx.roundRect({bounds: [62, 89, 66, 92], radius: 1, color: 0x17a7ffff});
    // Tama from the briefing to CORE.
    if (scene < 6) R.pet = tx.image({resource: pets, bounds: HOME, variant: 3, frame: 0});
    text([6, 116, 234, 125], 'SORTIE ' + sortie + '   ' + T[ch], 'caption', 0xd8ccb0ff);
    R.hint = text([6, 125, 234, 134], '', 'caption', 0x8a7c66ff, 33);
    R.lk = -1;
  }
  // The opaque image is last: at full size it exactly covers the set.
  img = tx.image({resource: res0, bounds: ZB, clip: FULL, sourceWidth: 240, sourceHeight: 135});
  mon = tx.image({resource: monRes, bounds: MON, clip: FULL, sourceWidth: 112, sourceHeight: 63});
  mon.setVisible(tx, false);
  // WAKE: Tama walks to the cushion in front of the TV (over its image).
  if (scene === 7) R.pet = tx.image({resource: pets, bounds: HOME, variant: 3, frame: 0});
  R.pf = 0;
  // Act I: the HUD boots on CALL once the camera is in the screen, a piece
  // every other frame; VOYAGE's dive takes it down again. LETHE's tag
  // follows its octahedron (patch).
  if (scene < 3) {
    const p = scene ? 0 : 1, e0 = scene > 1 ? 82 : 1e9;
    HUDI.split(',').forEach((e, i) => add(box(e), 36 * p + 2 * i * p, e0 + 2 * i));
    R.rd = RDB.map((b, i) => add(text(b, '', 'caption', i > 1 ? 0xd7f4ffff : 0x50ffc8ff, 5), (42 + 2 * i) * p, e0 + 3 * i));
    // It moves: its clip is the screen (setRect keeps the clip).
    R.tag = tx.text({bounds: [0, 0, 40, 9], clip: FULL, text: gb('LETHE'), font: 'caption', color: 0xf81fffff});
    R.tag.setVisible(tx, false);
    R.tgv = false;
  }
  if (S) R.say = add(text(S[0].split(' ').map(Number), '', 'caption', 0xffffffff, 40), +S[1]);
  if (ch < 4) R.card = add(text([8, 36, 232, 56], R.T = T[ch], 'display', INK[ch], 16), 4, 60);
  R.rv = R.cr = -1;
  if (scene > 3 && ch < 4) {
    R.score = add(text([4, 2, 150, 13], scene === 4 ? 'LETHE' : NAMES[scene], 'caption', 0xffffffff, 20));
    R.lamp = add(tx.roundRect({bounds: [226, 3, 236, 11], radius: 3, color: 0xfb47ffff}));
    R.lc = 0xfb47ffff; R.rc = 0x17a7ffff; R.k = -1;
    if (scene === 4) {
      R.ring = add(tx.strokeRect({bounds: [3, 83, 53, 133], width: 1, color: 0x17a7ffff}));
      // Tama on the comm window: the cat from the room.
      add(tx.strokeRect({bounds: [3, 15, 22, 34], width: 1, color: 0x17a7ffff}));
      add(tx.image({resource: pets, bounds: [4, 16, 21, 33], variant: 3, frame: 0}));
      R.lives = [0, 1, 2].map(i => add(tx.instantiate(lives, {offset: [160 + i * 12, 4]}), 0, i > 1 ? 48 : 1e9));
    } else {
      R.plate = add(tx.rect({bounds: [60, 118, 236, 133], color: 255}));
      add(tx.rect({bounds: [62, 120, 234, 131], color: 0xfa3fffff, opacity: 90}));
      tx.group(R.plate, 2, 170);
      R.meter = add(tx.gradient({bounds: [64, 128, 65, 130], axis: 'x', from: 0x17a7ffff, to: 0xfb47ffff}));
      R.stat = add(text([64, 119, 234, 128], 'SIGNAL', 'caption', 0xffffffff, 40));
      // Interference: bands of the image, torn sideways (shown by patch).
      R.tear = []; R.tv = [];
      for (let i = 0; i < 3; ++i) {
        R.tear.push(tx.image({resource: res0, bounds: FULL, clip: [0, 0, 240, 1], sourceWidth: 240, sourceHeight: 135}));
        R.tear[i].setVisible(tx, false);
      }
    }
  }
  // Shutters last: the set turns on and off from its middle line, HUD too.
  R.top = tx.rect({bounds: MON, clip: FULL, color: 255});
  R.bot = tx.rect({bounds: MON, clip: FULL, color: 255});
  R.top.setVisible(tx, false);
  R.bot.setVisible(tx, false);
  R.cv = 1;
  bt = null;
}
function patch(tx) {
  const on = zoom === 0, f = zoom === ZOOM, S = R.S;
  if (zoom !== zb) { zb = zoom; ZB = boundsAt(zoom); img.setRect(tx, ZB); }
  if (f !== vis) { img.setVisible(tx, !f); mon.setVisible(tx, f); vis = f; }
  // ZENITH's hit and CORE's burst shake the full-screen image for 12
  // frames (zb = -1 puts it back on the next frame).
  const k = scene === 4 ? t - 112 : scene === 5 ? t - 115 : -1;
  if (on && k >= 0 && k < 12) {
    const a = 6 * (1 - k / 12), x = rnd(a * M.sin(t * 2.7)), y = rnd(a * M.cos(t * 3.9));
    img.setRect(tx, [x, y, 240 + x, 135 + y]);
    zb = -1;
  }
  // Shutters: the TV turns on in the first news frames and off at the end
  // of CORE; in WAKE they are eyelids over the whole view.
  const o = scene === 0 ? clamp((t - 2) / 8) : scene === 5 ? clamp((140 - t) / 8) : scene === 7 ? lid(t) : 1;
  if (o < 1 || R.cv < 1) {
    const E = scene === 7 ? FULL : ZB, m = (E[1] + E[3]) / 2, h = (E[3] - E[1]) * o / 2;
    R.top.setRect(tx, [E[0], E[1], E[2], rnd(m - h)]);
    R.bot.setRect(tx, [E[0], rnd(m + h), E[2], E[3]]);
    if ((o < 1) !== (R.cv < 1)) { R.top.setVisible(tx, o < 1); R.bot.setVisible(tx, o < 1); }
    R.cv = o;
  }
  if (R.say) {
    let k = -1;
    for (let i = 1; i < S.length; i += 2) if (t >= +S[i]) k = i;
    if (k !== said && k > 0) R.say.setText(tx, S[k + 1]);
    said = k;
    // TWIST's lines type at 2 frames a letter (the storyboard).
    const n = k > 0 ? M.min(S[k + 1].length, (t - S[k]) >> (scene === 3)) : -1;
    if (n !== R.rv && n >= 0) R.say.setReveal(tx, n);
    R.rv = n;
  }
  for (let i = 0; i < hud.length; ++i) {
    const v = on && t >= hw[3 * i] && t < hw[3 * i + 1] ? 1 : 0;
    if (v !== hw[3 * i + 2]) { hud[i].setVisible(tx, v > 0); hw[3 * i + 2] = v; }
  }
  // Tama: in WAKE (after its face and the blink of sitting up) it walks to
  // the cushion; elsewhere it sits there and blinks.
  if (R.pet) {
    if (scene === 7) {
      const v = t >= WCUT;
      if (v !== R.pv) { R.pet.setVisible(tx, v); R.pv = v; }
      if (v) R.pet.setRect(tx, lerp4([150, 92, 182, 124], HOME, smooth(clamp((t - WCUT - 6) / 20))));
    }
    const b = t % 40 > 37 ? 1 : 0;
    if (b !== R.pf) { R.pet.setImageFrame(tx, 3, b); R.pf = b; }
  }
  // The hint drops ENTER ZOOM while Enter is dead.
  if (R.hint && +locked() !== R.lk) {
    R.lk = +locked();
    R.hint.setText(tx, (R.lk ? '            ' : 'ENTER ZOOM  ') + 'L/R CHAPTER  U/D LOAD');
  }
  if (R.card) {
    const n = M.max(0, M.min(R.T.length, (t - 4) >> 1));
    if (n !== R.cr) { R.card.setReveal(tx, n); R.cr = n; }
  }
  if (scene < 3) {
    // Readouts every other frame, and only on the full screen (the HUD).
    if (on && !(t & 1)) {
      R.rd[0].setText(tx, String(240 + rnd(BR[0] * 3) % 760));
      R.rd[1].setText(tx, String(rnd(BR[1] * 2) + 1e4).slice(1));
      R.rd[2].setText(tx, String(BR[2] * 7919 % 9000 + 1e3));
    }
    const g = on && TGX === TGX;
    if (g) R.tag.setRect(tx, [rnd(TGX) + 7, rnd(TGY) - 11, rnd(TGX) + 47, rnd(TGY) - 2]);
    if (g !== R.tgv) { R.tag.setVisible(tx, g); R.tgv = g; }
    return;
  }
  // TWIST goes on from VOYAGE's last frame (no iris). Its lines go black
  // in the negative past the wall (twist()).
  if (scene === 3) {
    const ng = t > 121 && t < 196;
    if (R.say && ng !== !!R.ng) { R.say.setColor(tx, ng ? 0x000000ff : 0xffffffff); R.ng = ng; }
    return;
  }
  if (scene > 5) return;
  // CORE's time stop: the HUD and the torn bands hold where they are.
  if (scene === 5 && t >= 100 && t < 112) return;
  if (!(t & 7)) R.score.setText(tx, (scene === 4 ? 'LETHE' : NAMES[scene]) + ' ' + String(1e6 + tick * 10).slice(1));
  const lc = t & (t > 71 ? 2 : 8) ? 0xfb47ffff : 0x167fffff;
  if (lc !== R.lc) { R.lamp.setColor(tx, lc); R.lc = lc; }
  if (scene === 4) {
    // The lives bob every 4 frames; the ring turns orange on the lock.
    const k = (t >> 2 & 1) | ((t + 3) >> 2 & 1) << 1 | ((t + 6) >> 2 & 1) << 2;
    if (k !== R.k) {
      R.k = k;
      for (let i = 0; i < 3; ++i) R.lives[i].place(tx, {offset: [160 + i * 12, 4 + ((t + i * 3) >> 2) % 2]});
    }
    const rc = t > 71 ? 0xfb47ffff : 0x17a7ffff;
    if (rc !== R.rc) { R.ring.setColor(tx, rc); R.rc = rc; }
  } else {
    const p = M.min(100, t);
    if (p !== R.k) { R.k = p; R.meter.setRect(tx, [64, 128, 65 + rnd(1.69 * p), 130]); }
    R.stat.setText(tx, 'SIGNAL ' + p + ' PLANS ' + live.length + ' DRAWS ' + nd);
    // Tear: each band shows when a per-frame hash is under the eased
    // interference level, at a hashed height and offset.
    const nz = on ? t < 100 ? ramp(t, 60, 100) ** 1.5 : t < 115 ? 0 : 1 - ramp(t, 115, 127) : 0;
    for (let i = 0; i < 3; ++i) {
      const r = R.tear[i], h = M.sin(t * 12.9898 + i * 78.233) * 43758.5453, u = h - M.floor(h), v = u < nz;
      if (v) {
        const y = rnd(u / nz * 124), dx = rnd(((u * 7) % 1 - .5) * 36 * nz);
        r.setRect(tx, [dx, 0, 240 + dx, 135]);
        r.setClip(tx, [0, y, 240, y + 3 + i * 4]);
      }
      if (v !== !!R.tv[i]) { r.setVisible(tx, v); R.tv[i] = v; }
    }
  }
}
