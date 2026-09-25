// The home screen's music player. Kasane owns the picture; pocket.overlay owns keys.
(function () {
  var host = pocket.overlay, ui = pocket.kasane;
  var view = ui.mount('music');
  var EXT = ['.mp3', '.wav', '.pok'];

  var p = null, sub = null, path = '', state = 'idle', busy = false;
  var outputProbe = null, outputBound = false;

  function leaf(s) { var i = s.lastIndexOf('/'); return i < 0 ? s : s.slice(i + 1); }
  function say(m) { view.set({message: m}); }

  function fail(where, e) {
    say(where + ' ' + ((e && e.code) || e));
    console.log('PLAYER_FAIL ' + where + ' ' + ((e && e.code) || e));
    busy = false;
  }

  function drop() {
    if (sub) { sub.close(); sub = null; }
    if (p) { p.close(); p = null; }
    state = 'idle';
  }

  function open(src, play) {
    drop();
    path = src; busy = true;
    view.set({title: leaf(src), message: 'OPENING'});
    pocket.audio.player.open({ source: src }).then(function (h) {
      p = h;
      view.bind('playback');
      console.log('PLAYER_OPEN ' + src + ' durationMs=' + h.info().durationMs);
      sub = h.onState(function (e) {
        state = e.state; say('');
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

  function chose(f) { busy = false; if (f) open(f, true); else say(p ? '' : 'ENTER TO CHOOSE'); }

  // The grant screen on its own key. README.md.
  function share() {
    busy = true; say('CHOOSING FOLDER');
    pocket.fs.requestFolder('sd').then(function (r) {
      if (!r) { busy = false; say('NO FOLDER SHARED'); return; }
      return pocket.fs.pickFile('sd', { extensions: EXT }).then(chose);
    }, function (e) { fail('SHARE', e); });
  }

  // pickFile first, the grant screen only if it refuses. README.md.
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

  host.onKey(function (e) {
    if (e.key === '?') { view.toggleHelp(); return; }
    // Diagnostic-only: the real music presenter stays on screen. The source
    // publishes from the audio task but this probe does not bind its fields.
    if (globalThis.KSN_P1_OUTPUT_OVERLAY_PROBE && e.action === 'up') {
      if (!outputProbe) {
        outputProbe = pocket.audio.outputSource({sampleMs: 33});
        console.log('KSN_P1_OUTPUT_OVERLAY ACTIVE sampleMs=33');
      }
      return;
    }
    if (globalThis.KSN_P1_OUTPUT_OVERLAY_PROBE && e.action === 'down') {
      if (outputProbe && !outputBound) {
        view.bind('output');
        outputBound = true;
        console.log('KSN_P1_OUTPUT_OVERLAY BOUND visible=1');
      }
      return;
    }
    if (busy) return;
    if (view.dismissHelp()) return;
    if (e.action === 'accept') { if (p) toggle(); else pick(); return; }
    if (e.action === 'right') { if (p) next(); return; }
    if (e.action === 'left') { pick(); return; }
    if (e.action === 'up') share();
  });

  view.set({title: 'NO TRACK', message: 'ENTER TO CHOOSE'});
  // Input and Promise callbacks own the JS work; native Kasane owns drawing.
  globalThis.frame = null;
})();
