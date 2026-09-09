// The home screen's music player. Overlay app -- no ui.*, no menu underneath.
// Why it is shaped this way: README.md. Terse because the guest parses this
// file and the bytes cost heap.
(function () {
  var o = pocket.overlay, W = o.region.width, H = o.region.height;
  var EXT = ['.mp3', '.wav', '.pok'];
  var MAX = pocket.capabilities.get('ui.overlay').limits.maxTextChars;

  var p = null, sub = null, path = '', state = 'idle', busy = false;
  var pos = 0, gaps = 0, msg = 'ENTER TO CHOOSE', title = 'NO TRACK';
  var dirty = true, total = null, tick = 0, help = false;

  // The face is 6 px for ASCII and 12 for everything else, and overlay.text
  // counts BYTES while String.slice counts code units. Both facts are needed
  // twice: to trim a name to the limit, and to put a box exactly round it.
  function walk(s, cap) {
    for (var b = 0, w = 0, i = 0; i < s.length; i++) {
      var c = s.charCodeAt(i), n = c < 128 ? 1 : c < 2048 ? 2 : 3;
      if (cap && b + n > MAX) break;
      b += n; w += c < 128 ? 6 : 12;
    }
    return cap ? s.slice(0, i) : w;
  }
  function cut(s) { return walk(s, 1); }
  function wpx(s) { return walk(s, 0); }

  // Text on its own black plate, sized to the text. No band across the top:
  // the ground belongs to the scene, and only the letters take any of it.
  function plate(x, y, s, r, g, b) {
    if (!s) return;
    s = cut(s);
    o.rect(x, y, wpx(s) + 8, 16, 0, 0, 0);
    o.text(x + 4, y + 2, s, r, g, b);
  }

  function leaf(s) { var i = s.lastIndexOf('/'); return i < 0 ? s : s.slice(i + 1); }
  function say(m) { msg = m; dirty = true; }

  function fail(where, e) {
    say(where + ' ' + ((e && e.code) || e));
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
    title = leaf(src); say('OPENING');
    pocket.audio.player.open({ source: src }).then(function (h) {
      p = h;
      // Real, from the file's own Xing/Info or VBRI tag, or null. Two different
      // pictures below, never one rounded into the other.
      total = h.info().durationMs;
      console.log('PLAYER_OPEN ' + src + ' durationMs=' + total);
      sub = h.onState(function (e) {
        state = e.state; say(''); dirty = true;
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
      if (n) open(n, true); else say('END OF FOLDER');
    }, function (e) { fail('NEXT', e); });
  }

  function chose(f) { busy = false; if (f) open(f, true); else dirty = true; }

  // The grant screen on its own key: once a folder is shared there is no
  // refusal left to reach it through, and a wrong choice would be permanent.
  function share() {
    busy = true; say('CHOOSING FOLDER');
    pocket.fs.requestFolder('sd').then(function (r) {
      if (!r) { busy = false; say('NO FOLDER SHARED'); return; }
      return pocket.fs.pickFile('sd', { extensions: EXT }).then(chose);
    }, function (e) { fail('SHARE', e); });
  }

  // pickFile first, the grant screen only if it refuses: the refusal is the
  // only signal for "no grant yet". Both are shell modals and win over us.
  function pick() {
    busy = true; say('CHOOSING');
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
    if (e.key === '?') { help = !help; dirty = true; return; }
    if (busy) return;
    if (help) { help = false; dirty = true; return; }
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

  // ONE status line. It used to be two -- a transport state and a note -- which
  // said "PLAYING" and "OPENING" a few pixels apart and left the reader to work
  // out which was current. A message wins while it stands, and the transport
  // speaks when there is nothing to report.
  function status() {
    if (msg) return msg;
    if (!p) return '';
    var t = state.toUpperCase() + '  ' + (pos / 1000 | 0) + 's';
    if (total) t += ' / ' + (total / 1000 | 0) + 's';
    if (gaps) t += '  GAPS ' + gaps;
    return t;
  }

  var HELP = ['ENTER  PLAY / PAUSE', 'LEFT   CHOOSE A FILE',
              'UP     CHOOSE A FOLDER', 'RIGHT  NEXT TRACK',
              '-  =   VOLUME', 'ESC    LEAVE THE PLAYER'];

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
    if (help) {
      o.rect(0, 0, W, H, 0, 0, 0);
      for (var i = 0; i < HELP.length; i++)
        o.text(14, 14 + i * 18, HELP[i], 226, 240, 255);
      o.text(14, H - 18, '?  CLOSE', 110, 140, 165);
      return;
    }
    plate(8, 8, title, 226, 240, 255);
    plate(8, 28, status(), 150, 190, 220);
    bar(H - 26);
    plate(8, H - 20, '?: help', 110, 140, 165);
  };
})();
