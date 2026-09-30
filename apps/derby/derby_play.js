'use strict';
// ---- Demo: DEMO_IDLE_S of no key at the paddock (wall clock), then the
// game plays itself, its keys fed to P(). Player state is set aside; races
// 1e6+n are the demo's seeds; silent (A = null); no save.
// GK by split, not [...'adesr1,/', 'tab']: that spread ran the guest out of
// heap on the device while evaluating (README).
// The curtain (README): FX = ms to black before the demo, ms held black,
// ms back in (also after a key), alpha steps.
const DEMO_IDLE_S = 15, DEMO_RES = 150, GK = 'a d e s r 1 , / tab'.split(' '), FX = [500, 700, 300, 8];
let dm = 0, dk = 0, dn = 0, idle = 0, kp = '', bk, fe = -1e9, fa = 0, nw = 0;
// A key ends the demo and is spent doing so (dk '' matches no key).
function attract(b) {
  const k = K.down().join(), n = nw = pocket.time ? pocket.time.now() : 0;
  let hit = b || k && k !== kp;
  for (const x of GK) hit = hit || K.pressed(x);
  kp = k;
  if (!dm) {
    dk = 0;
    if (!idle || hit || k || scene !== 'pad') idle = n;
    else if (n - idle >= DEMO_IDLE_S * 1e3) {
      // Its code loads now, under the black curtain; short of heap, the
      // paddock comes back and the idle clock starts over (README).
      try { pocket.app.load('demo'); } catch (e) { log('DEMOFAIL ' + e); return idle = n; }
      demo(1, n), fe = n + FX[1];
    }
    return;
  }
  if (hit || scene === 'res' && t > DEMO_RES) return fe = n, demo(0, n);
  pilot();
}
globalThis.frame = function (b) {
  if (b & 0x2000) { if (dm) demo(0); save(); return log('SAVE points=' + pts + ' race=' + raceNo); }
  try {
    attract(b);
    // The frame inline, not a function: ~150 B less resident (README).
    fr: {
      const P = k => dk === 0 ? K.pressed(k) : k === dk;
      ++t;
      if (camT > 0) --camT;
      // The curtain: wall clock only (attract's nw, fe); the game never reads it.
      fa = rnd(mx(0, mn(1, mx(dm ? 0 : (nw - idle + FX[0]) / FX[0] - DEMO_IDLE_S * 1e3 / FX[0], 1 - (nw - fe) / FX[2]))) * FX[3]) * 255 / FX[3] | 0;
      if (P('tab')) { tier = (tier + 1) % 3; const l = ['stands', 'crowd', 'pk'].filter(n => live[n] || queue.indexOf(n) >= 0); drop(l); want(l); log('TIER ' + tier); }
      load();
      let xs, c, l = pick, close = -1, gate = 0, extra = null;
      if (scene === 'pad') {
        if (P('a') || P('d')) { pick = (pick + (P('d') ? 1 : 7)) % 8; if (A) A.cue('move'); }
        stake = mx(50, mn(stake + (P('e') ? 50 : P('s') ? -50 : 0), 500, pts));
        if (P('1')) { log('PICK ' + (pick + 1) + ' stake=' + stake + ' odds=' + od[pick]); if (A) A.cue('accept'); { enter('gate'); break fr; } }
      } else if (scene === 'gate' || scene === 'race') {
        // A camera key overrides the director for 5 s.
        if (P(',') || P('/') || P('a') || P('d')) { cam = (cam + (P('/') || P('d') ? 1 : 2)) % 3; camT = 45; man = 150; }
        if (scene === 'gate' && (!queue.length && t > 40 || t > 300)) { log('GO' + (queue.length ? ' LOADSTALL' : '')); { enter('race'); break fr; } }
        if (scene === 'race') {
          const lead = mx.apply(null, rs.x);
          if (!slow && lead > D - 20) { slow = 1; camT = 60; log('SLOW'); }
          // Slow motion shows each sim step over 4 frames: the sim never
          // changes, so the camera cannot change the race.
          disp += slow ? .25 : 1;
          for (; disp >= 1 && rs.done < 3; --disp) {
            const was = rs.done;
            step(F, rs, DT);
            for (let i = 0; i < 8; ++i) ph[i] += rs.v[i] * DT / 6.5 * 2 * PI;
            const w = order(rs)[0];
            if (w !== ld) { if (lead > 500) log('LEAD #' + (w + 1) + ' at ' + rnd(lead) + 'M'); if (lead > 400) dl = 60; ld = w; }
            // The photo: every runner where it was when the winner crossed.
            if (!was && rs.done) photoX = at((rs.tc[w] - rs.t + DT) / DT);
          }
          if (live.gate && lead > 120) drop(['gate']);
          if (rs.done >= 3) { settle(); { enter('photo'); break fr; } }
        }
        xs = at(slow ? disp : 1);
        if (scene === 'gate') for (let i = 0; i < 8; ++i) if (!live['r' + i]) xs[i] = -999;
        if (hold > 0) --hold;
        if (dl > 0) --dl;
        if (man > 0) --man;
        // The director: a shot for each stretch of the race, a lead change
        // after 400 m cuts to the new leader, a cut is held 45 frames, HEAD ON
        // (a close finish) until the slow motion (README).
        // WIDE away from every panning unit is the nearest one (wide()).
        const o = ro = order(rs), L = rs.x[o[0]];
        let m = slow ? 3 : man ? cam : scene !== 'race' ? 0 : hold > 0 || cm === 6 ? cm :
          L < 150 ? 0 : L < 400 ? 2 : L < 700 ? (dl > 0 ? 1 : 0) : L < VS[0] - 60 ? 1 : L < VS[0] + 60 ? 5 :
          L > D - 70 && L - rs.x[o[1]] < 1.5 ? 6 : 0;
        if (!m && scene === 'race' && !live.gate) m = wide(L);
        if (m !== cm) { cl = man ? pick : o[0]; cm = m; hold = camT = 45; log('CAM ' + (NAMES[m] || m)); }
        c = shot(m === 6 ? 0 : m, xs, cl, scene === 'gate' || camT === 45);
        close = m === 1 ? cl : -1;
        gate = live.gate;
        extra = ['map', z8(0)];
        for (let i = 0; i < 8; ++i) extra[1][i] = 8 + mn(rs.x[i], D) * .224;
      } else if (scene === 'photo') {
        c = shot(4, photoX);
        xs = photoX;
        extra = ['photo', [120]];
        if (t > 110 || t > 20 && P('1')) { enter('res'); break fr; }
      } else if (scene === 'res') {
        if (P('1')) { ++raceNo; save(); { enter('pad'); break fr; } }
        if (P('r')) { replay = 1; { enter('gate'); break fr; } }
        l = fin.o[0];
        extra = ['conf', [t, mn(96, t)]];
      }
      if (!xs) {
        // Paddock warm-up and the winner's canter: a lone horse, camera locked.
        xs = z8(-999);
        xs[l] = 60 + t * .25;
        ph[l] += .25 / 6.5 * 2 * PI;
        c = shot(1, xs, l);
        close = l;
      }
      paint(c, xs, close, gate, extra);
      const up = tx => {
        hud(tx);
        R.dm.setVisible(tx, dm > 0 && !(t & 16));
        R.fd.setVisible(tx, fa > 0);
        R.fd.setColor(tx, fa);
        // The lettering follows the face (offsets LO from its left or right
        // edge and its top); its text is the HUD's metres and first three.
        const v = !!vr && von > 10 && vr[2] - vr[0] > 99;
        if (R.v && v !== R.vs) for (let i = 0; i < 3; ++i) R.v[i].setVisible(tx, R.vs = v);
        if (v) for (let i = 0; i < 3; ++i) R.v[i].setRect(tx, [vr[i && 2] + LO[i], vr[1] + LO[i + 3], vr[2] + LO[i + 6], vr[1] + LO[i + 9]]);
        if (v && !(t & 3)) { R.v[2].setColor(tx, t & 8 ? 0xff2020ff : 0x401010ff); R.v[0].setText(tx, R.t[1].s + ' ' + R.t[0].s.slice(0, 5)); }
        if (scene === 'photo') {
          // Magnify 2x about the line and the runners' feet.
          const z = mn(1, t / 30), e = z * z * (3 - 2 * z);
          R[0].setRect(tx, [rnd(-120 * e), rnd(-100 * e), rnd(240 + 120 * e), rnd(135 + 35 * e)]);
        }
      };
      if (need) { build(up); need = 0; } else V.patch(up);
    }
  } catch (e) { log('FRAMEFAIL ' + scene + ' ' + e); throw e; }
  sound();
};
enter('pad');
log('READY tones=' + !!A);
