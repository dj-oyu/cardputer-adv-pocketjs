// IMU axis calibration, Kasane port. Terse: guest parses this, bytes cost
// heap. See README.md for rationale.
(function () {
  const view = pocket.kasane;
  const state = {head: 'START', live: 'NO SAMPLE YET', stat: 'WAIT',
                 spin: 'GYR OFF', foot: 'HOLD STILL. ESC QUITS'};
  const F = [['head',[12,26,228,38],0xf0f8ffff,32],['live',[20,52,220,64],0x8ef0c4ff,32],
    ['stat',[20,66,220,78],0xa9bacaff,48],['spin',[20,80,220,92],0xffd479ff,32],
    ['foot',[12,104,228,116],0x8fa6bcff,32]];
  const scene = view.createScene({
    build: function (tx, s) {
      tx.background(0x071425ff);
      tx.text({bounds:[12,8,228,20],color:0x69cdeeff,text:'IMU AXIS CALIBRATION'});
      tx.roundRect({bounds:[12,48,228,94],radius:5,color:0x12334aff});
      var r = {}, i, d;
      for (i = 0; i < F.length; i++) { d = F[i];
        r[d[0]] = tx.text({bounds:d[1],color:d[2],text:s[d[0]],capacity:d[3]}); }
      return r;
    },
    patch: function (tx, r, s) {
      for (var i = 0; i < F.length; i++) r[F[i][0]].setText(tx, s[F[i][0]]);
    }
  });
  function say(key, t) { if (state[key] !== t) { state[key] = t; scene.invalidate(); } }
  function beep(hz) {
    pocket.audio.tone({ frequencyHz: hz, durationMs: 120, gain: 0.4 }).then(null, function () {});
  }
  function mm(v) { var n = Math.round(v * 1000); return (n < 0 ? '' : '+') + n; }

  var LABEL = ['DESK, FACE UP', 'DESK, FACE DOWN', 'UP, FACING YOU',
               'UP, INVERTED', 'RIGHT EDGE DOWN', 'LEFT EDGE DOWN'];
  var AXIS = [2, 2, 1, 1, 0, 0], SIGN = [1, -1, 1, -1, -1, 1];
  var NAME = ['AX', 'AY', 'AZ'], PUB = ['X', 'Y', 'Z'];

  var cap = pocket.capabilities.get('sensors.imu');
  console.log('IMUCAL CAP ' + cap.supported + ' ' + cap.available);
  if (!cap.supported || !cap.available) {
    say('head', 'NO IMU: ' + cap.reason);
    console.log('IMUCAL_UNAVAILABLE ' + cap.reason);
    globalThis.frame = function () { scene.flush(state); };
    scene.flush(state);
    return;
  }

  var now = null, recent = [], got = [], peak = [0, 0, 0], seen = 0;
  var idle = pocket.sensors.imu.latest();
  console.log('IMUCAL GYRO BEFORE ' + (idle && idle.gyro ? 'ON' : 'null'));

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
    say('head', ok ? 'DONE' : 'DONE, DISAGREED');
    say('live', t);
    say('stat', 'ERR ' + err + '%  GYR ' + mm(peak[0]) + ' ' + mm(peak[1]) + ' ' + mm(peak[2]));
    say('spin', 'SAVING');
    say('foot', 'ESC QUITS');
    pocket.storage.set('axes', { map: t, err: err, peak: peak, n: seen, ok: ok })
      .then(function () { say('spin', 'SAVED'); },
            function (e) { say('spin', 'SAVE ' + e.code); });
    sub.close();
    sub.close();          // close() twice is documented as a no-op
    checkAt = ticks + 10;
    beep(660);
  }

  var HOLD = 45, still = 0, armed = true, moved = 0, ticks = 0, checkAt = 0;

  function show() {
    if (got.length < 6) {
      say('head', (got.length + 1) + '/6 ' + LABEL[got.length]);
      say('foot', armed ? 'HOLD STILL. ESC QUITS' : 'MOVE TO NEXT');
    }
    if (!now) return;
    var a = now.accel, g = now.gyro;
    say('live', 'X' + mm(a.x) + ' Y' + mm(a.y) + ' Z' + mm(a.z));
    say('stat', 'MAG' + mm(Math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z)) +
              (armed ? (still ? ' HOLD ' + (HOLD - still) : ' MOVING') : ' MOVE ON') +
              ' DROP ' + now.dropped);
    say('spin', g ? 'GYR ' + mm(g.x) + ' ' + mm(g.y) + ' ' + mm(g.z) : 'GYR OFF');
  }

  function step() {
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
  }

  globalThis.frame = function () { step(); scene.flush(state); };

  show();
  scene.flush(state);
  console.log('IMUCAL_READY');
})();
