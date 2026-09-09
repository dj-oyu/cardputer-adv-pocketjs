// The home screen's music player. Overlay app: it ends XMB and stands in its
// place, so there is no ui.* here at all -- pocket.overlay is a display list the
// shell composites over the scene. Why it is shaped this way: README.md.
// Terse because the guest parses this file and the bytes cost heap.
(function () {
  var o = pocket.overlay, W = o.region.width;
  var EXT = ['.mp3', '.wav', '.pok'];

  var p = null, sub = null, path = '', state = 'idle', busy = false;
  var pos = 0, gaps = 0, note = 'PRESS ENTER TO CHOOSE', title = 'NO TRACK';
  var dirty = true, auto = true, total = null, tick = 0;

  function leaf(s) { var i = s.lastIndexOf('/'); return i < 0 ? s : s.slice(i + 1); }

  // overlay.text counts BYTES and String.slice counts code units, and a folder
  // of Japanese track names is where that difference stops being theoretical:
  // "02 インザハウス.mp3" is 14 characters and 28 bytes, so slice(0,23) passed a
  // string over the limit and the surface refused it -- correctly, since it
  // refuses rather than clips. This counts what the surface counts, from the
  // limit the surface publishes. A surrogate pair is charged 6 instead of 4,
  // which errs short, which is the safe side.
  var MAX = pocket.capabilities.get('ui.overlay').limits.maxTextChars;
  function cut(s) {
    for (var b = 0, i = 0; i < s.length; i++) {
      var c = s.charCodeAt(i);
      b += c < 128 ? 1 : c < 2048 ? 2 : 3;
      if (b > MAX) break;
    }
    return s.slice(0, i);
  }
  function set(t, n) { title = t; note = n; dirty = true; }

  function fail(where, e) {
    set(title, where + ' ' + ((e && e.code) || e));
    console.log('PLAYER_FAIL ' + where + ' ' + ((e && e.code) || e));
    busy = false;
  }

  function drop() {
    if (sub) { sub.close(); sub = null; }
    if (p) { p.close(); p = null; }
    state = 'idle'; pos = 0;
  }

  function open(src, play) {
    drop();
    path = src; busy = true;
    set(leaf(src), 'OPENING');
    pocket.audio.player.open({ source: src }).then(function (h) {
      p = h;
      // Real, from the file's own Xing/Info or VBRI tag, or null when the
      // encoder wrote neither. Two different pictures, below, because a bar
      // with no end is not a shorter bar -- it is a different claim.
      total = h.info().durationMs;
      console.log('PLAYER_OPEN ' + src + ' durationMs=' + total);
      sub = h.onState(function (e) {
        state = e.state; dirty = true;
        if (e.state === 'error') { fail('PLAYBACK', e.error); return; }
        // The end of a track is where the queue happens, and it is the only
        // place: nothing holds a list, so "what is next" is asked once, of the
        // host, at the moment it matters.
        if (e.state === 'ended' && auto) next();
      });
      busy = false;
      if (play) h.play().then(null, function (e) { fail('PLAY', e); });
    }, function (e) { fail('OPEN', e); });
  }

  // fs.nextFile walks the folder for us: one pass, no list, and the same idea
  // of "a playable file" the picker used. wrap makes the folder a loop.
  function next() {
    if (!path) return;
    busy = true;
    pocket.fs.nextFile(path, { extensions: EXT, wrap: true }).then(function (n) {
      busy = false;
      if (n) open(n, true); else set(title, 'END OF FOLDER');
    }, function (e) { fail('NEXT', e); });
  }

  function chose(f) { busy = false; if (f) open(f, true); else dirty = true; }

  // pickFile first and the grant screen only if it refuses: there is no query
  // for "do I have a grant", so the refusal is the signal. Both screens are
  // MODALS -- they take the whole panel and the keyboard, and this guest is not
  // ticked while one is up (3.1: shell modals beat the overlay).
  function pick() {
    busy = true;
    set(title, 'CHOOSING');
    pocket.fs.pickFile('sd', { extensions: EXT }).then(chose, function (e) {
      if (!e || e.code !== 'PERMISSION_DENIED') { fail('PICK', e); return; }
      pocket.fs.requestFolder('sd').then(function (r) {
        if (!r) { busy = false; set('NO TRACK', 'NO FOLDER SHARED'); return; }
        return pocket.fs.pickFile('sd', { extensions: EXT }).then(chose);
      }, function (er) { fail('SHARE', er); });
    });
  }

  o.onKey(function (e) {
    if (busy) return;
    if (e.action === 'accept') { if (p) toggle(); else pick(); return; }
    if (e.action === 'right') { if (p) next(); return; }
    if (e.action === 'left') { pick(); return; }
  });

  function toggle() {
    busy = true;
    (state === 'playing' ? p.pause() : p.play())
      .then(function () { busy = false; }, function (e) { fail('TRANSPORT', e); });
  }

  // Two pictures, and which one is drawn is decided by whether the file said
  // how long it is -- never by rounding one into the other.
  function bar(y) {
    var track = W - 24;
    o.rect(12, y, track, 2, 24, 38, 54);
    if (total) {
      // How far through. Only drawn when there is a real end to be a fraction
      // of; clamped because the last frames can report past the tag's total.
      var w = (pos * track / total) | 0;
      if (w > track) w = track;
      if (w > 0) o.rect(12, y, w, 2, 120, 200, 255);
      return;
    }
    // No length in the file. A light travels the track instead of filling it:
    // it says "playing" without ever implying a position, which a partly full
    // bar cannot help doing. Three segments of falling brightness make the
    // direction readable at 30 fps without any animation state of its own --
    // tick is the frame counter, and the position is a function of it.
    if (state !== 'playing') return;
    var seg = 18, span = track + seg * 3;
    var head = (tick * 3) % span - seg * 3;
    var c = [[120, 200, 255], [60, 120, 170], [30, 60, 90]];
    for (var i = 0; i < 3; i++) {
      var x = head + i * seg, w2 = seg;
      if (x < 0) { w2 += x; x = 0; }
      if (x + w2 > track) w2 = track - x;
      if (w2 > 0) o.rect(12 + x, y, w2, 2, c[i][0], c[i][1], c[i][2]);
    }
  }

  var n = 0;
  globalThis.frame = function () {
    if (p && !(++n % 5)) {
      var s = p.status();
      if (s.positionMs !== pos || s.underruns !== gaps) dirty = true;
      pos = s.positionMs; gaps = s.underruns;
    }
    // The travelling light is the one thing here that has to be redrawn on a
    // frame where nothing else changed, so it is what marks the display dirty
    // -- and therefore what makes the whole list be rebuilt and recomposited.
    //
    // Every OTHER frame, and the honest note is that this DID NOT recover the
    // frame rate it was written to recover. The home screen runs at 26 fps
    // while a card track plays, against 30 idle, and halving this changed
    // nothing measurable: the cost is in `send` (7.1 -> 8.8 ms) and `prep`,
    // which is the panel transfer and the scene competing with the decoder and
    // the card, not this. 15 Hz is kept because it is free and smooth enough
    // for a moving band, NOT because it bought anything.
    if (!total && state === 'playing' && !(n % 2)) { tick++; dirty = true; }
    if (!dirty) return;
    dirty = false;
    o.begin();
    o.rect(0, 0, W, 36, 6, 12, 22);
    o.rect(0, 36, W, 1, 40, 70, 100);
    o.text(8, 4, cut(title), 226, 240, 255);
    o.text(8, 20, state.toUpperCase(), 130, 190, 230);
    o.text(72, 20, (pos / 1000 | 0) + 's' +
                   (total ? ' / ' + (total / 1000 | 0) + 's' : ''), 150, 170, 190);
    if (gaps) o.text(130, 20, 'GAPS ' + gaps, 255, 170, 90);
    bar(42);
    o.text(8, 50, cut(note), 122, 150, 175);
    // Two lines, and the constraint is PIXELS rather than bytes: the region
    // refuses a box that does not fit, ASCII is 6 px wide, and 8 + 44*6 is 272
    // against a 240 px panel. The byte cap and the panel width are different
    // limits and the smaller one is not always the same one.
    o.text(8, 104, 'ENTER PLAY   LEFT PICK', 90, 110, 130);
    o.text(8, 118, 'RIGHT NEXT   ESC LEAVE', 90, 110, 130);
  };
})();
