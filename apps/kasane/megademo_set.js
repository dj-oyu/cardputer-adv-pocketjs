// MEGADEMO chunk 4: the view's state, the plan loader and the set's build
// and patch (megademo_view.js has its tables).
'use strict';
let tier = KN.length - 1, scene = 0, t = 0, tick = 0, live = [], pend = [], next = null, loaded = 0;
let img, mon, hud = [], hw = [], R = {}, built = -1, vis = false, said = -1;
let zoom = ZOOM, target = ZOOM, spd = 2, pin = false, fixed = false, held = 0;
const drop = h => { for (let i = 0; i < h.length; ++i) H.unregister(h[i]); };
const reg = th => { const p = th(); ++loaded; return p[1] ? H.register(p[0], p[1]) : H.register(p[0]); };
const lerp4 = (a, b, e) => a.map((v, j) => rnd(v + (b[j] - v) * e)), clamp = v => M.max(0, M.min(1, v));
function prefetch(s, max) {
  if (next && (next.s !== s || next.k !== tier)) { drop(next.h); next = null; }
  if (!next) next = {s: s, k: tier, h: [], sp: specs(s, tier)};
  while (max-- > 0 && next.h.length < next.sp.length && MEM.info().internalFreeBytes >= LOAD[2])
    next.h.push(reg(next.sp[next.h.length]));
}
// fresh: drop the prefetched set too (tier change, recovery). The old set
// goes before the rest registers: they overlap only as far as the
// memory gate let the prefetch run.
function enter(s, fresh) {
  if (fresh && next) { drop(next.h); next = null; }
  prefetch(s, 0);
  const old = live.length;
  if (s === 7 && scene !== 7) ++sortie;
  drop(live); live = next.h; pend = next.sp.slice(live.length); next = null; scene = s; t = 0;
  console.log('MEGADEMO SCENE ' + NAMES[s] + ' tier=' + tier + ' plans=' + live.length +
    ' freed=' + old + ' registered=' + loaded);
}
const LBL = s => s === 4 ? 'LETHE' : NAMES[s];
// WAKE's eyelids (0 shut .. 1 open): open halfway, a blink, open; shut and
// open again for sitting up.
const lid = t => t < 10 ? 0 : t < 18 ? .55 * smooth((t - 10) / 8) : t < 21 ? .55 - (t - 18) / 6 :
  t < 30 ? .05 + .95 * smooth((t - 21) / 9) : t < 40 ? 1 : t < 43 ? (43 - t) / 3 : t < 48 ? (t - 43) / 5 : 1;
const boundsAt = step => step === 0 ? FULL : step === ZOOM ? MON :
  lerp4(FULL, MON, step / ZOOM * step / ZOOM * (3 - 2 * step / ZOOM));
function build(tx, room) {
  hud = []; hw = []; R = {}; vis = false; said = -1;
  // A HUD ref shows on the full screen while from <= t < to.
  const add = (r, from, to) => { hud.push(r); hw.push(from || 0, to || 1e9, 1); return r; };
  const text = (b, s, f, c, n) => tx.text({bounds: b, text: s, font: f, color: c, capacity: n});
  const box = s => {
    const e = s.split(' '), b = e.slice(0, 4).map(Number), c = parseInt(e[4], 16), r = +e[5] || 0;
    return e[5] > 'A' ? text(b, e[5].replace('_', ' '), 'caption', c) :
      r > 0 ? tx.roundRect({bounds: b, radius: r, color: c}) : r < 0 ?
        tx.strokeRect({bounds: b, width: -r, color: c}) : tx.rect({bounds: b, color: c, opacity: +e[6] || 255});
  };
  const ch = CHAP[scene], S = R.S = SAY[scene] && SAY[scene].split('|'), T = TITLE.split('|');
  tx.background(255);
  if (room) {
    tx.gradient({bounds: [0, 0, 240, 100], axis: 'y', from: 0x0a0618ff, to: GLOW[ch], dither: true});
    ROOM.split(',').forEach(box);
    // A tally on the wall, a mark a night (three before the first).
    for (let i = 0; i < M.min(12, sortie - 739); ++i) {
      const x = 198 + i * 3 + (i >> 2) * 3;
      tx.rect({bounds: [x, 18, x + 1, 28], color: 0x8a7c66ff});
    }
    R.led = tx.roundRect({bounds: [62, 89, 66, 92], radius: 1, color: 0x17a7ffff});
    // The body and Tama from the briefing to CORE; in LOST the room is empty
    // (where did Tama go?); in WAKE the viewer is the body.
    if (scene < 6) {
      R.body = BODY.map(b => tx.roundRect({bounds: b.slice(0, 4), radius: b[4], color: b[5]}));
      R.pet = tx.image({resource: pets, bounds: HOME, variant: 3, frame: 0});
    }
    text([6, 116, 234, 125], 'SORTIE ' + sortie + '   ' + T[ch], 'caption', 0xd8ccb0ff);
    text([6, 125, 234, 134], 'ENTER ZOOM  L/R CHAPTER  U/D LOAD', 'caption', 0x8a7c66ff);
  }
  // The opaque image is last: at full size it exactly covers the set.
  img = tx.image({resource: res0, bounds: boundsAt(zoom), clip: FULL, sourceWidth: 240, sourceHeight: 135});
  mon = tx.image({resource: monRes, bounds: MON, clip: FULL, sourceWidth: 112, sourceHeight: 63});
  mon.setVisible(tx, false);
  // WAKE: Tama walks to the cushion in front of the TV (over its image).
  if (scene === 7) R.pet = tx.image({resource: pets, bounds: HOME, variant: 3, frame: 0});
  if (room && scene === 6) R.off = text([96, 46, 150, 58], 'NO SIGNAL', 'caption', 0xffffffff);
  if (scene < 3) {
    const N = NEWS.replace('~', sortie).split('|').map(gb);
    TV.split(',').forEach(e => add(box(e)));
    R.head = add(text([6, 114, 234, 124], N[2 * scene], 'caption', 0xffffffff, 40));
    add(text([6, 124, 234, 134], N[2 * scene + 1], 'caption', 0x80d9e8ff));
    R.N = N;
  }
  if (S) R.say = add(text(S[0].split(' ').map(Number), '', 'caption', 0xffffffff, 40), +S[1]);
  if (ch < 4) R.card = add(text([8, 36, 232, 56], R.T = T[ch], 'display', INK[ch], 16), 4, 60);
  if (scene > 3 && ch < 4) {
    R.score = add(text([4, 2, 150, 13], LBL(scene), 'caption', 0xffffffff, 20));
    R.lamp = add(tx.roundRect({bounds: [226, 3, 236, 11], radius: 3, color: 0xfb47ffff}));
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
}
function patch(tx, f) {
  const on = zoom === 0, fix = zoom === ZOOM, B = boundsAt(zoom), S = R.S;
  // Interference in the last news beat, full screen only.
  const j = on && scene === 2 && t >= 16 && (t * 7) % 5 < 2 ? (t * 3) % 7 - 3 : 0;
  img.setRect(tx, [B[0] + j, B[1], B[2] + j, B[3]]);
  if (fix !== vis) { img.setVisible(tx, !fix); mon.setVisible(tx, fix); vis = fix; }
  // Shutters: the TV turns on in the first news frames and off at the end
  // of CORE; in WAKE they are eyelids over the whole view.
  const o = scene === 0 ? clamp((t - 2) / 8) : scene === 5 ? clamp((140 - t) / 8) : scene === 7 ? lid(t) : 1;
  if (o < 1 || R.cv < 1) {
    const E = scene === 7 ? FULL : B, m = (E[1] + E[3]) / 2, h = (E[3] - E[1]) * o / 2;
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
    if (k > 0) R.say.setReveal(tx, M.min(S[k + 1].length, t - S[k]));
  }
  for (let i = 0; i < hud.length; ++i) {
    const v = on && t >= hw[3 * i] && t < hw[3 * i + 1] ? 1 : 0;
    if (v !== hw[3 * i + 2]) { hud[i].setVisible(tx, v > 0); hw[3 * i + 2] = v; }
  }
  // Tama: in WAKE (after its face, act2's wake(), and the blink of sitting
  // up) it walks to the cushion; elsewhere it sits there and blinks.
  if (R.pet) {
    R.pet.setRect(tx, scene !== 7 ? HOME : lerp4([150, 92, 182, 124], HOME, smooth(clamp((t - 46) / 20))));
    R.pet.setImageFrame(tx, 3, t % 40 > 37 ? 1 : 0);
    if (scene === 7) R.pet.setVisible(tx, t >= 42);
  }
  // The body sleeps on from the news's dive; it flinches when LETHE hits.
  if (R.body) for (let i = 0; i < 2; ++i) {
    const b = BODY[i], d = scene === 4 && t >= 48 && t < 52 ? (t & 1) * 2 - 3 : 0;
    R.body[i].setRect(tx, [b[0], b[1] + d, b[2], b[3] + d]);
    R.body[i].setVisible(tx, scene > 0 || t >= 14);
  }
  if (R.off) {
    R.off.setVisible(tx, fix && t >= 20 && (t & 8) > 0);
    R.led.setColor(tx, t & 16 ? 0xfb47ffff : 0x401010ff);
  }
  if (R.card) R.card.setReveal(tx, M.max(0, M.min(R.T.length, (t - 4) >> 1)));
  if (scene < 3) {
    if (scene === 2 && t >= 16) R.head.setText(tx, R.N[t & 4 ? 4 : 6]);
    return;
  }
  if (scene === 3) {
    const e = M.min(1, t / 20), w = 20 + 100 * e, h = 12 + 56 * e;
    img.setClip(tx, [rnd(120 - w), rnd(67 - h), rnd(120 + w), rnd(68 + h)]);
    return;
  }
  if (scene > 5) return;
  // CORE's time stop: the HUD and the torn bands hold where they are.
  if (scene === 5 && t >= 100 && t < 112) return;
  if (!(t & 7)) R.score.setText(tx, NAMES[scene] + ' ' + String(1e6 + tick * 10).slice(1));
  R.lamp.setColor(tx, t & (t > 71 ? 2 : 8) ? 0xfb47ffff : 0x167fffff);
  if (scene === 4) {
    for (let i = 0; i < 3; ++i) R.lives[i].place(tx, {offset: [160 + i * 12, 4 + ((t + i * 3) >> 2) % 2]});
    R.ring.setColor(tx, t > 71 ? 0xfb47ffff : 0x17a7ffff);
  } else {
    const n = f ? f.d.length : 0, p = M.min(100, t);
    R.meter.setRect(tx, [64, 128, 65 + rnd(1.69 * p), 130]);
    R.stat.setText(tx, 'SIGNAL ' + p + ' PLANS ' + live.length + ' DRAWS ' + n);
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
