// The home screen's music player. Overlay app: it ends XMB and stands in its
// place, so there is no ui.* here at all -- pocket.overlay is a display list the
// shell composites over the scene. Why it is shaped this way: README.md.
// Terse because the guest parses this file and the bytes cost heap.
(function () {
  var o = pocket.overlay, W = o.region.width;
  var EXT = ['.mp3', '.wav', '.pok'];

  var p = null, sub = null, path = '', state = 'idle', busy = false;
  var pos = 0, gaps = 0, note = 'PRESS ENTER TO CHOOSE', title = 'NO TRACK';
  var dirty = true, auto = true;

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
      console.log('PLAYER_OPEN ' + src);
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

  // A bar rather than a number, because durationMs is null for MP3 until the
  // track ends -- there is no total to be a fraction of. This fills over one
  // minute and repeats, so it says "still going" and never says "how far".
  function bar(y) {
    var w = ((pos % 60000) * (W - 24) / 60000) | 0;
    o.rect(12, y, W - 24, 2, 24, 38, 54);
    if (w > 0) o.rect(12, y, w, 2, 120, 200, 255);
  }

  var n = 0;
  globalThis.frame = function () {
    if (p && !(++n % 5)) {
      var s = p.status();
      if (s.positionMs !== pos || s.underruns !== gaps) dirty = true;
      pos = s.positionMs; gaps = s.underruns;
    }
    if (!dirty) return;
    dirty = false;
    o.begin();
    o.rect(0, 0, W, 36, 6, 12, 22);
    o.rect(0, 36, W, 1, 40, 70, 100);
    o.text(8, 4, cut(title), 226, 240, 255);
    o.text(8, 20, state.toUpperCase(), 130, 190, 230);
    o.text(72, 20, (pos / 1000 | 0) + 's', 150, 170, 190);
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
