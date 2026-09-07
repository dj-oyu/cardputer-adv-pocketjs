// pocket.app / pocket.time / pocket.log self-check. APPCHK lines go to USB,
// log tag `js`. What each one proves is in README.md beside this file.
(function () {
  var T = pocket.time, L = pocket.log, frames = 0;
  function say(t) { console.log('APPCHK ' + t); }
  function cap(n) {
    var c = pocket.capabilities.get(n);
    say('CAP ' + n + ' ' + c.supported + '/' + c.available +
        ' ' + JSON.stringify(c.limits));
  }
  function tail() {
    var a = 0, last = null, p;
    do {
      p = L.read({ afterSequence: a, limit: 8 });
      if (p.records.length) { last = p.records[p.records.length - 1]; a = last.sequence; }
    } while (p.records.length === 8);
    return { last: last, dropped: p.dropped };
  }
  function logCheck() {
    var long = new Array(200).join('xy');   // 398 bytes
    L.write('warn', 'short line');
    L.write('debug', long);
    var t = tail().last;
    say('LOG ' + t.level + ' bytes=' + t.text.length + ' trunc=' + t.truncated);
    try { L.write('shout', 'x'); } catch (e) { say('LOG LEVEL ' + e.code); }
    var p = L.read({ limit: 8 }), seen = 0;
    for (var i = 0; i < p.records.length; i++)
      if (p.records[i].text.indexOf('APPCHK') === 0) seen++;
    say('LOG CONSOLE ' + seen + '/' + p.records.length);
    for (var j = 0; j < 24; j++) L.write('info', long);
    say('LOG FLOOD dropped=' + tail().dropped);
  }
  pocket.app.start({
    start: function () {
      cap('app'); cap('time'); cap('log');
      say('FRAME ' + typeof globalThis.frame);
      var w = T.wall(), t0 = T.now();
      say('WALL ' + w.source + ' ' + w.unixMs);
      return T.sleep(150).then(function () {
        say('SLEEP ' + Math.round(T.now() - t0) + 'ms frames=' + frames);
        var s = pocket.cancel.source(), p = T.sleep(500, { cancel: s.token });
        s.cancel();
        return p.then(function () { say('CANCEL NOT REJECTED'); },
                      function (e) { say('CANCEL ' + e.code + ' ' + e.outcome); });
      }).then(function () {
        return T.sleep(500, { timeoutMs: 60 }).then(
          function () { say('TIMEOUT NOT REJECTED'); },
          function (e) { say('TIMEOUT ' + e.code + ' ' + e.outcome); });
      }).then(function () {
        return T.sleep(-1).then(null, function (e) { say('BADARG ' + e.code); });
      });
    },
    stop: function (r) { say('STOP ' + r + ' frames=' + frames); }
  });
  pocket.app.onFrame(function (f) {
    if (++frames === 1) say('FIRST t=' + Math.round(f.timeMs) + ' d=' + f.deltaMs);
    if (frames !== 30) return;
    say('FRAME30 delta=' + Math.round(f.deltaMs));
    logCheck();
    var m = pocket.device.metrics();
    say('METRICS heap=' + m.heapFreeBytes + ' big=' + m.largestFreeBlockBytes +
        ' js=' + m.jsHeapBytes + ' fps=' + Math.round(m.fps) + ' io=' + m.ioDropped);
    pocket.app.exit();
    say('AFTER EXIT');
  });
})();
