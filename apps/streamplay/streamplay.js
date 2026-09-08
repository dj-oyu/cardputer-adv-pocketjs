// Streamed playback, and the frame cost of feeding it. See README.md --
// including why this file is terse: the guest parses it, so bytes cost heap.
(function () {
  var P = { w: 1, h: 2, pos: 24, top: 25, left: 28, bg: 64, r: 68, fg: 96 };
  function node(k, x, y, w, h, c, t) {
    var id = ui.createNode(k);
    ui.setProp(id, P.pos, 1);
    ui.setProp(id, P.left, x); ui.setProp(id, P.top, y);
    ui.setProp(id, P.w, w); ui.setProp(id, P.h, h);
    ui.setProp(id, k === 1 ? P.fg : P.bg, c);
    if (t) ui.setText(id, t);
    ui.insertBefore(1, id, 0);
    return id;
  }
  var shown = {};
  function say(id, t) { if (shown[id] !== t) { shown[id] = t; ui.setText(id, t); } }

  ui.setProp(1, P.bg, 0x071425ff);
  node(1, 12, 8, 216, 10, 0x69cdeeff, 'STREAMED PLAYBACK');
  var head = node(1, 12, 26, 216, 12, 0xf0f8ffff, 'OPENING');
  var live = node(1, 12, 46, 216, 10, 0x8ef0c4ff, '');
  var rate = node(1, 12, 62, 216, 10, 0xffd479ff, '');
  node(1, 12, 104, 216, 10, 0x8fa6bcff, 'ESC QUITS');

  var SRC = 'assets:/chime.wav';
  var IDLE = 60;
  var cap = pocket.capabilities.get('audio.playback');
  console.log('STREAM CAP ' + cap.supported + ' ' + cap.available);

  var p = null, sub = null, state = 'ready', done = false, ready = false;
  var cycle = 0, phase = 0, idleMs = 0, idleN = 0, playMs = 0, playN = 0;
  var last = 0, worst = 0;

  function fail(e) {
    var c = e && e.code ? e.code : e;
    say(head, 'FAILED'); say(live, String(c));
    console.log('STREAM_FAIL ' + c);
    phase = 3;
  }

  function open() {
    done = false; ready = false;
    idleMs = 0; idleN = 0; playMs = 0; playN = 0; worst = 0; phase = 0;
    pocket.audio.player.open({ source: SRC }).then(function (h) {
      p = h;
      var i = h.info();
      say(head, i.codec + ' ' + i.durationMs + 'ms');
      if (!cycle) {
        console.log('STREAM OPEN codec=' + i.codec + ' rate=' + i.sampleRate +
                    ' durationMs=' + i.durationMs + ' seekable=' + i.seekable);
      }
      sub = h.onState(function (e) {
        state = e.state;
        console.log('STREAM STATE ' + e.state +
                    (e.error ? ' error=' + e.error.code : ''));
        if (e.state === 'ended' || e.state === 'error') done = true;
      });
      ready = true;
    }, fail);
  }
  open();

  globalThis.frame = function () {
    if (phase === 3) return;
    var now = Date.now();
    if (!last) { last = now; return; }
    var d = now - last; last = now;

    if (phase === 0) {
      // Idle: a player is open and holds nothing but a path. The ring is not
      // allocated until the first play, so these frames are the baseline.
      idleMs += d; idleN++;
      say(live, 'IDLE ' + idleN + '/' + IDLE);
      if (idleN >= IDLE && ready) {
        phase = 1;
        p.play().then(null, fail);
      }
      return;
    }
    playMs += d; playN++;
    if (d > worst) worst = d;
    var s = p ? p.status() : null;
    if (s && (playN % 15) === 0) {
      say(live, s.state + ' ' + s.positionMs + 'ms');
      say(rate, 'UNDERRUNS ' + s.underruns);
      console.log('STREAM T pos=' + s.positionMs + ' underruns=' + s.underruns +
                  ' state=' + s.state);
    }
    if (!done) return;

    var u = s ? s.underruns : -1;
    // Tenths of a ms, integer-only: no float formatting to depend on.
    var a = Math.round(idleMs * 10 / idleN), b = Math.round(playMs * 10 / playN);
    say(head, 'CYCLE ' + cycle + ' DONE');
    say(live, 'IDLE ' + a / 10 + 'ms  PLAY ' + b / 10 + 'ms');
    say(rate, 'UNDERRUNS ' + u + '  WORST ' + worst + 'ms');
    console.log('STREAM DONE cycle=' + cycle + ' state=' + state +
                ' underruns=' + u +
                ' idleFrames=' + idleN + ' idleTenthMs=' + a +
                ' playFrames=' + playN + ' playTenthMs=' + b +
                ' deltaTenthMs=' + (b - a) + ' worstFrameMs=' + worst);
    if (sub) { sub.close(); sub = null; }
    if (p) { p.close(); p = null; }
    // An ended player is not replayable by contract, so a repeat is a fresh
    // open. That is also what makes each cycle an independent measurement.
    cycle++;
    open();
  };
})();
