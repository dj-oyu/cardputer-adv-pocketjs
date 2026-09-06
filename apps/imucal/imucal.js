// IMU axis calibration, and the first program written against pocket.*.
//
// How the BMI270 sits on the board is in none of the documents we have, and no
// amount of software can find it out: the only way is to hold the device in a
// known orientation and see which component gravity lands in. This walks six of
// them, records one reading each, and prints the three macros motion.c needs.
//
// It doubles as the acceptance test for pocket.sensors.imu, so it checks what
// the capability claims instead of assuming it.
//
// Text is upper case and short on purpose. Every distinct character a program
// shows is rebuilt into a font atlas slot, and the atlas rebuild is the largest
// single allocation a small app makes; the guest heap here runs at about 105 KB
// of its 128 KB with the largest free block near 13 KB.
(function () {
  var P = { width: 1, height: 2, position: 24, top: 25, left: 28,
    background: 64, radius: 68, color: 96 };
  function node(kind, x, y, w, h, color, text) {
    var id = ui.createNode(kind);
    ui.setProp(id, P.position, 1);
    ui.setProp(id, P.left, x); ui.setProp(id, P.top, y);
    ui.setProp(id, P.width, w); ui.setProp(id, P.height, h);
    ui.setProp(id, kind === 1 ? P.color : P.background, color);
    if (text) ui.setText(id, text);
    ui.insertBefore(1, id, 0);
    return id;
  }
  // Only when it changed: setText rebuilds the atlas whenever the string brings
  // a character the slot has not seen, and repaints regardless.
  var shown = {};
  function say(id, text) {
    if (shown[id] === text) return;
    shown[id] = text;
    ui.setText(id, text);
  }

  // Gravity points at the floor, so the axis reading +G is the one aimed at the
  // ceiling. AXIS/SIGN are the published axis and sign that should carry it.
  var LABEL = ['DESK, SCREEN UP', 'DESK, SCREEN DOWN', 'UPRIGHT, FACING YOU',
               'UPRIGHT, INVERTED', 'RIGHT EDGE DOWN', 'LEFT EDGE DOWN'];
  var AXIS = [2, 2, 1, 1, 0, 0];
  var SIGN = [1, -1, 1, -1, -1, 1];
  var NAME = ['AX', 'AY', 'AZ'];
  var PUB = ['X', 'Y', 'Z'];

  ui.setProp(1, P.background, 0x071425ff);
  node(1, 12, 8, 216, 10, 0x69cdeeff, 'IMU AXIS CALIBRATION');
  var head = node(1, 12, 28, 216, 12, 0xf0f8ffff, 'START');
  var panel = node(0, 12, 50, 216, 34, 0x12334aff);
  ui.setProp(panel, P.radius, 5);
  var live = node(1, 20, 57, 200, 10, 0x8ef0c4ff, 'NO SAMPLE YET');
  var state = node(1, 20, 71, 200, 10, 0xa9bacaff, 'WAIT');
  var foot = node(1, 12, 96, 216, 10, 0x8fa6bcff, 'ENTER RECORDS, ESC QUITS');

  function mm(v) { var n = Math.round(v * 1000); return (n < 0 ? '' : '+') + n; }

  var cap = pocket.capabilities.get('sensors.imu');
  console.log('IMUCAL CAP ' + cap.supported + ' ' + cap.available + ' ' +
              cap.reason + ' ' + JSON.stringify(cap.limits));
  if (!cap.supported || !cap.available) {
    say(head, 'NO IMU: ' + cap.reason);
    say(foot, 'ESC QUITS');
    console.log('IMUCAL_UNAVAILABLE ' + cap.reason);
    globalThis.frame = function () {};
    return;
  }

  // A published limit the API does not enforce is decoration. One hertz over
  // the rate the sensor is read at has to be refused at open.
  try {
    pocket.sensors.imu.watch({ rateHz: (cap.limits.maxRateHz || 0) + 1 }, function () {});
    console.log('IMUCAL_LIMIT_NOT_ENFORCED');
  } catch (e) {
    console.log('IMUCAL LIMIT ' + e.code + ' ' + e.operation + ' ' + e.retryable);
  }

  var current = null, recent = [], got = [];

  // 20 Hz, not the sensor's 50: this only has to look live, and every delivery
  // is a call into JS on the frame's own task.
  var sub = pocket.sensors.imu.watch({ rateHz: 20 }, function (s) {
    current = s;
    recent.push(s.accel);
    if (recent.length > 12) recent.shift();
  });

  // Still enough to trust: every axis inside a narrow band for the last 0.6 s.
  // 0.25 m/s2 is about 2.5% of gravity, above the part's noise and well below a
  // drifting hand.
  function steady() {
    if (recent.length < 12) return false;
    for (var k = 0; k < 3; k++) {
      var key = k === 0 ? 'x' : k === 1 ? 'y' : 'z';
      var lo = recent[0][key], hi = lo;
      for (var n = 1; n < recent.length; n++) {
        var v = recent[n][key];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
      }
      if (hi - lo > 0.25) return false;
    }
    return true;
  }

  function show() {
    if (got.length < 6) say(head, (got.length + 1) + '/6 ' + LABEL[got.length]);
    if (!current) return;
    var a = current.accel;
    say(live, 'X' + mm(a.x) + ' Y' + mm(a.y) + ' Z' + mm(a.z));
    var m = Math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    say(state, 'MAG' + mm(m) + (steady() ? ' STEADY' : ' MOVING') +
               ' DROP ' + current.dropped);
  }

  function dominant(v) {
    var i = Math.abs(v[1]) > Math.abs(v[0]) ? 1 : 0;
    if (Math.abs(v[2]) > Math.abs(v[i])) i = 2;
    return { i: i, sign: v[i] < 0 ? -1 : 1, size: Math.abs(v[i]) };
  }

  function report() {
    // Each published axis was observed twice, expecting +G once and -G once.
    // Both must name the same board component with opposite signs, or the
    // device was not held as asked and the answer is worth nothing.
    var map = [null, null, null], ok = true, scale = 0, s, d, e, seen;
    for (s = 0; s < 6; s++) {
      d = dominant(got[s]);
      scale += d.size;
      e = { i: d.i, sign: d.sign * SIGN[s] };
      seen = map[AXIS[s]];
      if (!seen) map[AXIS[s]] = e;
      else if (seen.i !== e.i || seen.sign !== e.sign) {
        ok = false;
        console.log('IMUCAL_DISAGREE ' + PUB[AXIS[s]] + ' ' + NAME[seen.i] + ' ' + NAME[e.i]);
      }
    }
    scale /= 6;
    var err = Math.round((scale / 9.80665 - 1) * 1000) / 10;
    console.log('IMUCAL SCALE ' + Math.round(scale * 1000) + ' EXPECT 9807 ERR ' + err + '%');
    if (map[0].i === map[1].i || map[1].i === map[2].i || map[0].i === map[2].i) {
      ok = false;
      console.log('IMUCAL_DEGENERATE two published axes came from one board axis');
    }
    var text = '';
    for (s = 0; s < 3; s++) {
      var body = (map[s].sign < 0 ? '-' : '') + NAME[map[s].i].toLowerCase();
      console.log('IMUCAL_MAP #define MAP_' + PUB[s] + '(ax,ay,az) (' + body + ')');
      text += (s ? ' ' : '') + PUB[s] + '=' + (map[s].sign < 0 ? '-' : '') + NAME[map[s].i];
    }
    console.log(ok ? 'IMUCAL_OK' : 'IMUCAL_SUSPECT redo what disagreed');
    say(head, ok ? 'DONE, SEE USB LOG' : 'DONE, READINGS DISAGREED');
    say(live, text);
    say(state, 'SCALE ERROR ' + err + '%');
    say(foot, 'ESC QUITS');
    sub.close();
    sub.close();          // documented as a no-op; if it were not, this throws
    console.log('IMUCAL_CLOSED');
  }

  var previous = 0;
  globalThis.frame = function (buttons) {
    var hit = (buttons & 0x4000) && !(previous & 0x4000);
    previous = buttons;
    if (got.length >= 6) return;
    if (hit && current && steady()) {
      var a = current.accel;
      got.push([a.x, a.y, a.z]);
      console.log('IMUCAL_SAMPLE ' + got.length + ' ' + mm(a.x) + ' ' + mm(a.y) + ' ' + mm(a.z));
      recent = [];
      if (got.length === 6) { report(); return; }
    } else if (hit) {
      console.log('IMUCAL_NOT_STEADY');
    }
    show();
  };

  show();
  console.log('IMUCAL_READY');
})();
