// IMU axis calibration: six orientations decide MAP_X/MAP_Y/MAP_Z for
// main/motion.c. Records on stillness, answers in sound, keeps the result.
// Why, and why this file is terse, are in README.md beside it.
(function () {
  var P = { w: 1, h: 2, pos: 24, top: 25, left: 28, bg: 64, r: 68, fg: 96 };
  function node(kind, x, y, w, h, color, text) {
    var id = ui.createNode(kind);
    ui.setProp(id, P.pos, 1);
    ui.setProp(id, P.left, x); ui.setProp(id, P.top, y);
    ui.setProp(id, P.w, w); ui.setProp(id, P.h, h);
    ui.setProp(id, kind === 1 ? P.fg : P.bg, color);
    if (text) ui.setText(id, text);
    ui.insertBefore(1, id, 0);
    return id;
  }
  var shown = {};
  function say(id, t) { if (shown[id] !== t) { shown[id] = t; ui.setText(id, t); } }
  function beep(hz) {
    pocket.audio.tone({ frequencyHz: hz, durationMs: 120, gain: 0.4 })
      .then(null, function () {});
  }
  function mm(v) { var n = Math.round(v * 1000); return (n < 0 ? '' : '+') + n; }

  var LABEL = ['DESK, SCREEN UP', 'DESK, SCREEN DOWN', 'UPRIGHT, FACING YOU',
               'UPRIGHT, INVERTED', 'RIGHT EDGE DOWN', 'LEFT EDGE DOWN'];
  var AXIS = [2, 2, 1, 1, 0, 0], SIGN = [1, -1, 1, -1, -1, 1];
  var NAME = ['AX', 'AY', 'AZ'], PUB = ['X', 'Y', 'Z'];

  ui.setProp(1, P.bg, 0x071425ff);
  node(1, 12, 8, 216, 10, 0x69cdeeff, 'IMU AXIS CALIBRATION');
  var head = node(1, 12, 26, 216, 12, 0xf0f8ffff, 'START');
  var panel = node(0, 12, 48, 216, 46, 0x12334aff);
  ui.setProp(panel, P.r, 5);
  var live = node(1, 20, 54, 200, 10, 0x8ef0c4ff, 'NO SAMPLE YET');
  var stat = node(1, 20, 68, 200, 10, 0xa9bacaff, 'WAIT');
  var spin = node(1, 20, 82, 200, 10, 0xffd479ff, 'GYR OFF');
  var foot = node(1, 12, 104, 216, 10, 0x8fa6bcff, 'HOLD STILL. ESC QUITS');

  var cap = pocket.capabilities.get('sensors.imu');
  console.log('IMUCAL CAP ' + cap.supported + ' ' + cap.available);
  if (!cap.supported || !cap.available) {
    say(head, 'NO IMU: ' + cap.reason);
    console.log('IMUCAL_UNAVAILABLE ' + cap.reason);
    globalThis.frame = function () {};
    return;
  }

  var now = null, recent = [], got = [], peak = [0, 0, 0], seen = 0;
  var idle = pocket.sensors.imu.latest();
  console.log('IMUCAL GYRO BEFORE ' + (idle && idle.gyro ? 'ON' : 'null'));

  // How the answer leaves the device. The walk needs the cable unplugged, so
  // the log is only read afterwards, by which time that run is over.
  pocket.storage.get('axes').then(function (r) {
    if (r) console.log('IMUCAL_LAST ' + r.value.map + ' err=' + r.value.err +
                       '% gyr=' + r.value.n + (r.value.ok ? ' ok' : ' SUSPECT'));
  }, function (e) { console.log('IMUCAL_LAST_FAILED ' + e.code); });

  var sub = pocket.sensors.imu.watch({ rateHz: 20 }, function (s) {
    now = s;
    recent.push([s.accel.x, s.accel.y, s.accel.z]);
    if (recent.length > 12) recent.shift();
    if (s.gyro) {
      seen++;
      var g = [s.gyro.x, s.gyro.y, s.gyro.z], k;
      for (k = 0; k < 3; k++) if (Math.abs(g[k]) > peak[k]) peak[k] = Math.abs(g[k]);
    }
  });

  function steady() {
    if (recent.length < 12) return false;
    for (var k = 0; k < 3; k++) {
      var lo = recent[0][k], hi = lo, n, v;
      for (n = 1; n < 12; n++) {
        v = recent[n][k];
        if (v < lo) lo = v; else if (v > hi) hi = v;
      }
      if (hi - lo > 0.25) return false;
    }
    return true;
  }
  function dominant(v) {
    var i = Math.abs(v[1]) > Math.abs(v[0]) ? 1 : 0;
    if (Math.abs(v[2]) > Math.abs(v[i])) i = 2;
    return { i: i, s: v[i] < 0 ? -1 : 1, size: Math.abs(v[i]) };
  }

  function report() {
    var map = [null, null, null], ok = true, scale = 0, i, d, e, was, t = '';
    for (i = 0; i < 6; i++) {
      d = dominant(got[i]); scale += d.size;
      e = { i: d.i, s: d.s * SIGN[i] };
      was = map[AXIS[i]];
      if (!was) map[AXIS[i]] = e;
      else if (was.i !== e.i || was.s !== e.s) {
        ok = false;
        console.log('IMUCAL_DISAGREE ' + PUB[AXIS[i]] + ' ' + NAME[was.i] + ' ' + NAME[e.i]);
      }
    }
    scale /= 6;
    var err = Math.round((scale / 9.80665 - 1) * 1000) / 10;
    console.log('IMUCAL SCALE ' + Math.round(scale * 1000) + ' ERR ' + err + '%');
    if (map[0].i === map[1].i || map[1].i === map[2].i || map[0].i === map[2].i) {
      ok = false;
      console.log('IMUCAL_DEGENERATE');
    }
    for (i = 0; i < 3; i++) {
      console.log('IMUCAL_MAP #define MAP_' + PUB[i] + '(ax,ay,az) (' +
                  (map[i].s < 0 ? '-' : '') + NAME[map[i].i].toLowerCase() + ')');
      t += (i ? ' ' : '') + PUB[i] + '=' + (map[i].s < 0 ? '-' : '') + NAME[map[i].i];
    }
    console.log(ok ? 'IMUCAL_OK' : 'IMUCAL_SUSPECT');
    // No cable can be attached through six positions, so the answer has to
    // survive without one: on screen in full, and in storage for later.
    say(head, ok ? 'DONE' : 'DONE, READINGS DISAGREED');
    say(live, t);
    say(stat, 'ERR ' + err + '%  GYR ' + mm(peak[0]) + ' ' + mm(peak[1]) + ' ' + mm(peak[2]));
    say(spin, 'SAVING');
    say(foot, 'ESC QUITS');
    pocket.storage.set('axes', { map: t, err: err, peak: peak, n: seen, ok: ok })
      .then(function () { say(spin, 'SAVED'); },
            function (e) { say(spin, 'SAVE ' + e.code); });
    sub.close();
    sub.close();          // documented no-op; if it were not, this throws
    checkAt = ticks + 10;
    beep(660);
  }

  var HOLD = 45, still = 0, armed = true, moved = 0, ticks = 0, checkAt = 0;

  function show() {
    if (got.length < 6) {
      say(head, (got.length + 1) + '/6 ' + LABEL[got.length]);
      say(foot, armed ? 'HOLD STILL. ESC QUITS' : 'MOVE TO THE NEXT POSITION');
    }
    if (!now) return;
    var a = now.accel, g = now.gyro;
    say(live, 'X' + mm(a.x) + ' Y' + mm(a.y) + ' Z' + mm(a.z));
    say(stat, 'MAG' + mm(Math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z)) +
              (armed ? (still ? ' HOLD ' + (HOLD - still) : ' MOVING') : ' MOVE ON') +
              ' DROP ' + now.dropped);
    say(spin, g ? 'GYR ' + mm(g.x) + ' ' + mm(g.y) + ' ' + mm(g.z) : 'GYR OFF');
  }

  globalThis.frame = function () {
    ticks++;
    if (checkAt && ticks >= checkAt) {
      checkAt = 0;
      var after = pocket.sensors.imu.latest();
      console.log('IMUCAL GYRO AFTER ' + (after && after.gyro ? 'STILL ON' : 'null'));
    }
    if (got.length >= 6) return;
    if (armed) {
      still = now && steady() ? still + 1 : 0;
      if (still >= HOLD) {
        var a = now.accel;
        got.push([a.x, a.y, a.z]);
        console.log('IMUCAL_SAMPLE ' + got.length + ' ' + mm(a.x) + ' ' +
                    mm(a.y) + ' ' + mm(a.z));
        beep(1046);
        // recent is kept: clearing it reads as movement to the re-arm below.
        still = 0; armed = false; moved = 0;
        if (got.length === 6) { report(); return; }
      }
    } else {
      moved = steady() ? 0 : moved + 1;
      if (moved >= 10) { armed = true; still = 0; beep(523); }
    }
    show();
  };

  show();
  console.log('IMUCAL_READY');
})();
