// Opus playback, and what the decode task costs the drawing task. See README.md.
// Terse because the guest parses this file and the bytes cost heap.
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

  ui.setProp(1, P.bg, 0x0b1a12ff);
  node(1, 12, 8, 216, 10, 0x8ef0c4ff, 'OPUS PLAYBACK');
  var head = node(1, 12, 26, 216, 12, 0xf0f8ffff, 'OPENING');
  var live = node(1, 12, 46, 216, 10, 0x69cdeeff, '');
  var rate = node(1, 12, 62, 216, 10, 0xffd479ff, '');
  node(1, 12, 104, 216, 10, 0x8fa6bcff, 'ESC QUITS');

  // Two materials, alternating, because they catch different failures and
  // neither covers the other. The click train hears timing; the tone is the only
  // one that can hear an underrun ANYWHERE -- see README.md.
  var SRCS = ['assets:/chime.pok', 'assets:/tone.pok'];
  var src = SRCS[0];
  var IDLE = 60;
  var cap = pocket.capabilities.get('audio.playback');
  console.log('OPUS CAP ' + cap.supported + ' ' + cap.available +
              ' codecs=' + cap.limits.codecs);

  var p = null, sub = null, state = 'ready', done = false, ready = false;
  var cycle = 0, phase = 0, idleMs = 0, idleN = 0, playMs = 0, playN = 0;
  var last = 0, worst = 0;

  function fail(e) {
    var c = e && e.code ? e.code : e;
    say(head, 'FAILED'); say(live, String((e && e.message) || c));
    console.log('OPUS_FAIL ' + c + ' ' + ((e && e.message) || ''));
    phase = 3;
  }

  function open() {
    done = false; ready = false;
    idleMs = 0; idleN = 0; playMs = 0; playN = 0; worst = 0; phase = 0;
    src = SRCS[cycle % SRCS.length];
    pocket.audio.player.open({ source: src }).then(function (h) {
      p = h;
      var i = h.info();
      say(head, (src.indexOf('tone') < 0 ? 'CLICKS ' : 'TONE ') + i.durationMs + 'ms');
      // Every cycle, not just the first: the source alternates now, so which
      // material a cycle used is part of reading its result.
      console.log('OPUS OPEN src=' + src + ' codec=' + i.codec +
                  ' rate=' + i.sampleRate + ' durationMs=' + i.durationMs +
                  ' seekable=' + i.seekable);
      sub = h.onState(function (e) {
        state = e.state;
        console.log('OPUS STATE ' + e.state + (e.error ? ' error=' + e.error.code : ''));
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
      // Idle: the player holds a path and nothing else. No rings, no decoder,
      // no task. These frames are the baseline the play frames are read against,
      // and the difference between them is the ONLY measurement anybody has of
      // what a decode task at priority 6 costs the renderer.
      idleMs += d; idleN++;
      say(live, 'IDLE ' + idleN + '/' + IDLE);
      if (idleN >= IDLE && ready) { phase = 1; p.play().then(null, fail); }
      return;
    }
    playMs += d; playN++;
    if (d > worst) worst = d;
    var s = p ? p.status() : null;
    if (s && (playN % 15) === 0) {
      say(live, s.state + ' ' + s.positionMs + 'ms');
      say(rate, 'UNDERRUNS ' + s.underruns);
      console.log('OPUS T pos=' + s.positionMs + ' underruns=' + s.underruns +
                  ' state=' + s.state);
    }
    if (!done) return;

    var u = s ? s.underruns : -1;
    var a = Math.round(idleMs * 10 / idleN), b = Math.round(playMs * 10 / playN);
    say(head, 'CYCLE ' + cycle + ' DONE');
    say(live, 'IDLE ' + a / 10 + 'ms  PLAY ' + b / 10 + 'ms');
    say(rate, 'UNDERRUNS ' + u + '  WORST ' + worst + 'ms');
    console.log('OPUS DONE cycle=' + cycle + ' src=' + src + ' state=' + state +
                ' underruns=' + u +
                ' idleFrames=' + idleN + ' idleTenthMs=' + a +
                ' playFrames=' + playN + ' playTenthMs=' + b +
                ' deltaTenthMs=' + (b - a) + ' worstFrameMs=' + worst);
    if (sub) { sub.close(); sub = null; }
    if (p) { p.close(); p = null; }
    // A fresh open each cycle, which is what makes every cycle independent: a
    // delta that grew across cycles would mean the decoder or a ring leaks
    // through a player's lifetime. One cycle cannot tell those apart.
    cycle++;
    open();
  };
})();
