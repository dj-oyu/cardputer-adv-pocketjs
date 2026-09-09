// Plays one file off the card. Why it is shaped this way: README.md.
// Terse because the guest parses this file and the bytes cost heap.
(function () {
  var P = { w: 1, h: 2, pos: 24, top: 25, left: 28, bg: 64, fg: 96 };
  function node(x, y, w, h, c, t) {
    var id = ui.createNode(1);
    ui.setProp(id, P.pos, 1);
    ui.setProp(id, P.left, x); ui.setProp(id, P.top, y);
    ui.setProp(id, P.w, w); ui.setProp(id, P.h, h);
    ui.setProp(id, P.fg, c);
    if (t) ui.setText(id, t);
    ui.insertBefore(1, id, 0);
    return id;
  }
  var shown = {};
  function say(id, t) { if (shown[id] !== t) { shown[id] = t; ui.setText(id, t); } }

  ui.setProp(1, P.bg, 0x0a1420ff);
  node(12, 6, 216, 10, 0x7fd6ffff, 'MUSIC');
  var name = node(12, 22, 216, 12, 0xf0f8ffff, 'CHECKING');
  var line = node(12, 42, 216, 10, 0xa8c4dcff, '');
  var meta = node(12, 58, 216, 10, 0xffd479ff, '');
  node(12, 104, 216, 10, 0x6d8296ff, 'ENTER PLAY/PAUSE  RIGHT PICK  ESC QUIT');

  // What this host actually decodes, which source_parse() settles by magic:
  // POK1 (Opus CELT), MPEG Layer III, WAV. Ogg .opus is not one of them, so
  // it is not offered -- a filter that lists files that cannot open is worse
  // than a narrower one.
  var EXT = ['.mp3', '.wav', '.pok'];
  var p = null, sub = null, path = '', state = 'idle', busy = true, ended = false;

  function fail(where, e) {
    var c = e && e.code ? e.code : e;
    say(name, where);
    say(line, String((e && e.message) || c));
    console.log('PLAYER_FAIL ' + where + ' ' + c);
    busy = false;
  }

  // The last component, because the whole path does not fit and the folder is
  // the one the person shared -- they already know which one it is.
  function leaf(s) { var i = s.lastIndexOf('/'); return i < 0 ? s : s.slice(i + 1); }

  function drop() {
    if (sub) { sub.close(); sub = null; }
    if (p) { p.close(); p = null; }
    state = 'idle'; ended = false;
  }

  function open(src) {
    drop();
    path = src; busy = true;
    say(name, leaf(src)); say(line, 'OPENING'); say(meta, '');
    pocket.audio.player.open({ source: src }).then(function (h) {
      p = h;
      var i = h.info();
      // durationMs is null for MP3 until it ends, and seekable is false there.
      // Saying so beats showing "0ms" as though it were a measurement.
      say(meta, i.codec + '  ' + (i.durationMs === null ? 'LENGTH UNKNOWN'
                                                        : i.durationMs + 'ms'));
      console.log('PLAYER_OPEN ' + JSON.stringify(i));
      sub = h.onState(function (e) {
        state = e.state;
        if (e.state === 'ended') { ended = true; say(line, 'ENDED'); }
        if (e.state === 'error') fail('PLAYBACK', e.error);
      });
      busy = false;
      h.play().then(null, function (e) { fail('PLAY', e); });
    }, function (e) { fail('OPEN', e); });
  }

  function chose(f) {
    if (!f) { say(name, 'NOTHING CHOSEN'); say(line, 'RIGHT TO PICK'); busy = false; return; }
    open(f);
  }

  // pickFile FIRST, and the grant screen only if it refuses. The refusal is
  // the reliable way to ask "is there a grant": there is no query for it, and
  // asking for a folder every time would put a permission screen in front of
  // somebody who already answered it once this session.
  function pick() {
    busy = true;
    say(line, 'CHOOSING');
    pocket.fs.pickFile('sd', { extensions: EXT }).then(chose, function (e) {
      if (!e || e.code !== 'PERMISSION_DENIED') { fail('PICK', e); return; }
      pocket.fs.requestFolder('sd').then(function (root) {
        if (!root) { say(name, 'NO FOLDER SHARED'); say(line, 'RIGHT TO PICK'); busy = false; return; }
        return pocket.fs.pickFile('sd', { extensions: EXT }).then(chose);
      }, function (er) { fail('SHARE', er); });
    });
  }

  var cap = pocket.capabilities.get('audio.playback');
  if (!cap.supported) { say(name, 'NO PLAYBACK'); say(line, 'ESC QUITS'); busy = false; }
  else {
    console.log('PLAYER CAP codecs=' + cap.limits.codecs);
    pick();
  }

  pocket.input.onAction(function (e) {
    if (e.phase !== 'press' || busy) return;
    if (e.action === 'right') { pick(); return; }
    if (e.action !== 'accept' || !p) return;
    // Resume from pause decodes from the head to rebuild the bit reservoir, so
    // for MP3 this gets slower the later in the song it happens. That is the
    // format, not this app; the status line is where the wait shows up.
    if (ended) { open(path); return; }
    busy = true;
    var next = state === 'playing' ? p.pause() : p.play();
    next.then(function () { busy = false; }, function (er) { fail('TRANSPORT', er); });
  });

  var n = 0;
  globalThis.frame = function () {
    if (!p || ++n % 10) return;
    var s = p.status();
    say(line, s.state.toUpperCase() + '  ' + s.positionMs + 'ms  GAPS ' + s.underruns);
  };
})();
