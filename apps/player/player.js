// The home screen's music player. Overlay app -- no ui.*, no menu underneath.
// Why it is shaped this way: README.md. Terse because the guest parses this
// file and the bytes cost heap; growing it by 2.4 KB once cost the session
// enough room to build pocket.fs on the first keypress.
(function () {
  var o = pocket.overlay, W = o.region.width;
  var EXT = ['.mp3', '.wav', '.pok'];
  var MAX = pocket.capabilities.get('ui.overlay').limits.maxTextChars;

  var p = null, sub = null, path = '', state = 'idle', busy = false;
  var pos = 0, gaps = 0, note = 'PRESS ENTER TO CHOOSE', title = 'NO TRACK';
  var dirty = true, total = null, tick = 0;

  // overlay.text counts BYTES; String.slice counts code units. README.md.
  function cut(s) {
    for (var b = 0, i = 0; i < s.length; i++) {
      var c = s.charCodeAt(i);
      b += c < 128 ? 1 : c < 2048 ? 2 : 3;
      if (b > MAX) break;
    }
    return s.slice(0, i);
  }
  function leaf(s) { var i = s.lastIndexOf('/'); return i < 0 ? s : s.slice(i + 1); }
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
      // Real, from the file's own Xing/Info or VBRI tag, or null. Two different
      // pictures below, never one rounded into the other.
      total = h.info().durationMs;
      console.log('PLAYER_OPEN ' + src + ' durationMs=' + total);
      sub = h.onState(function (e) {
        state = e.state; dirty = true;
        if (e.state === 'error') { fail('PLAYBACK', e.error); return; }
        if (e.state === 'ended') next();
      });
      busy = false;
      if (play) h.play().then(null, function (e) { fail('PLAY', e); });
    }, function (e) { fail('OPEN', e); });
  }

  // The queue, asked of the host at the only moment it matters. Nothing here
  // holds a list; fs.nextFile walks the folder in one pass. README.md.
  function next() {
    if (!path) return;
    busy = true;
    pocket.fs.nextFile(path, { extensions: EXT, wrap: true }).then(function (n) {
      busy = false;
      if (n) open(n, true); else set(title, 'END OF FOLDER');
    }, function (e) { fail('NEXT', e); });
  }

  function chose(f) { busy = false; if (f) open(f, true); else dirty = true; }

  // The grant screen, on its own key. It is NOT only reachable through a
  // refusal: once a folder is shared there is no refusal left to trigger it,
  // and a person who shared the wrong one would have had no way back.
  function share() {
    busy = true;
    set(title, 'CHOOSING FOLDER');
    pocket.fs.requestFolder('sd').then(function (r) {
      if (!r) { busy = false; set(title, 'NO FOLDER SHARED'); return; }
      return pocket.fs.pickFile('sd', { extensions: EXT }).then(chose);
    }, function (e) { fail('SHARE', e); });
  }

  // pickFile first, the grant screen only if it refuses: the refusal is the
  // only signal for "no grant yet". Both are shell modals and win over us.
  function pick() {
    busy = true;
    set(title, 'CHOOSING');
    pocket.fs.pickFile('sd', { extensions: EXT }).then(chose, function (e) {
      if (e && e.code === 'PERMISSION_DENIED') share(); else fail('PICK', e);
    });
  }

  function toggle() {
    busy = true;
    (state === 'playing' ? p.pause() : p.play())
      .then(function () { busy = false; }, function (e) { fail('TRANSPORT', e); });
  }

  o.onKey(function (e) {
    if (busy) return;
    if (e.action === 'accept') { if (p) toggle(); else pick(); return; }
    if (e.action === 'right') { if (p) next(); return; }
    if (e.action === 'left') { pick(); return; }
    if (e.action === 'up') share();
  });

  // A bar against a real end, or a light that travels and claims no position.
  function bar(y) {
    var track = W - 24;
    o.rect(12, y, track, 2, 24, 38, 54);
    if (total) {
      var w = (pos * track / total) | 0;
      if (w > track) w = track;
      if (w > 0) o.rect(12, y, w, 2, 120, 200, 255);
      return;
    }
    if (state !== 'playing') return;
    var seg = 18, span = track + seg * 3, head = (tick * 3) % span - seg * 3;
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
    // The light is the only thing that must redraw on an otherwise still frame,
    // so it is what marks the list dirty. 15 Hz; it bought no frames, README.md.
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
    o.text(8, 104, 'ENTER PLAY  LEFT PICK  UP FOLDER', 90, 110, 130);
    o.text(8, 118, 'RIGHT NEXT  ESC LEAVE  -/= VOL', 90, 110, 130);
  };
})();
