// MEGADEMO chunk 6: the first scene (WAKE, the prologue) and frame().
'use strict';
enter(7);
let boot = true;
const DRAW = (k, a) => { const h = live[k]; if (h) H.draw(h, a); };
globalThis.frame = function (buttons) {
  buttons |= 0;
  if (boot) {
    // The frames (3 x 10 KB) and the first scene's plans on the first
    // frame, after the compile's memory is gone (during evaluation the
    // CORE build found no 10 KB holes on the device, 2026-10-28).
    boot = false;
    H.beginFrame(0);
    while (pend.length) live.push(reg(pend.shift()));
  }
  const press = buttons & ~held;
  held = buttons;
  // Enter: full screen <-> the monitor in the room, except while the
  // story owns the camera (locked(), set). Into I the camera dives into the
  // screen; on waking, Tama's face over the whole view, then a cut to the
  // room. A pinned room is let go when a locked stretch starts: CORE's
  // critical beat pulls the view back into the TV. WAKE's t < 2: a scene
  // starts after this check, so its t = 0 goes by unseen here.
  const lk = locked();
  if (press & 0x4000 && !lk) { target = target ? 0 : ZOOM; pin = target > 0; spd = 1; }
  if (lk && pin) { pin = false; target = 0; spd = scene === 5 ? 2 : ZOOM; }
  if (scene === 7 && (t < 2 || t === WCUT) && !pin) { target = t === WCUT ? ZOOM : 0; spd = ZOOM; }
  if (!scene && t === 14 && !pin) { target = 0; spd = 2; }
  zoom += M.max(-spd, M.min(spd, target - zoom));
  let f = null;
  try {
    // UP/DOWN: load tier. LEFT/RIGHT: previous/next scene.
    const k = tier + (press & 0x10 ? 1 : press & 0x40 ? -1 : 0);
    if (k !== tier && k >= 0 && k < KN.length) { tier = k; enter(scene, 1); }
    if (press & 0xa0) enter((scene + (press & 0x20 ? 1 : NAMES.length - 1)) % NAMES.length);
    else if (t >= PLAY[scene]) enter((scene + 1) % NAMES.length);
    // A plan decodes in ~13 ms on the device: one a frame, this scene's
    // first, then the next scene's over the last frames. Before the
    // frame: a park here must not show a new image under the old view.
    let b = LOAD[1];
    for (; b > 0 && pend.length; --b) live.push(reg(pend.shift()));
    if (b && t >= PLAY[scene] - LOAD[0]) prefetch((scene + 1) % NAMES.length, b);
    // WAKE and Act I draw as they go (pocket.shapes: no list kept), so
    // their frame begins first; TWIST draws over its plans in post().
    if (scene === 7 || scene < 3) {
      H.beginFrame(scene === 7 ? wakeB(t % LEN[7]) : briefB(scene, t));
      f = frameOf(scene, tier, t % LEN[scene], DRAW);
    } else {
      f = frameOf(scene, tier, t % LEN[scene], DRAW);
      H.beginFrame(f.b);
      for (let i = 0; i < f.d.length; ++i) DRAW(f.d[i][0], f.d[i][1]);
      if (f.post) f.post();
    }
    H.commit();
  } catch (e) {
    if (!tier) throw e;
    --tier;
    console.log('MEGADEMO DEGRADE tier=' + tier + ' ' + e);
    enter(scene, 1);
    f = null;
  }
  // A set per scene and tier, rebuilt when the room comes into view.
  const room = zoom > 0, key = scene + 8 * tier + 64 * room;
  if (built !== key) {
    // Same tx: a scene's first frame must not show build()'s defaults.
    V.replace(tx => (build(tx, room), patch(tx, f)));
    built = key;
  } else {
    // The set built at t = 0 has been presented by now.
    if (t === 1) console.log('MEGADEMO VIEW ' + NAMES[scene] + ' commands=' + V.stats().displayed.commands);
    V.patch(tx => patch(tx, f));
  }
  if ((zoom === ZOOM) !== fixed) {
    fixed = !fixed;
    console.log('MEGADEMO IMAGE ' + (fixed ? 'FIXED_PIE' : 'DYNAMIC_STRETCH'));
  }
  ++t; ++tick;
};
