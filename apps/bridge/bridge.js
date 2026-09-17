(function () {
  var state = {st: 'CONNECTING...', info: '', job: '', seen: ''};
  function T(tx, x, y, w, h, c, t, cap) {
    return tx.text({bounds: [x, y, x + w, y + h], font: 'caption', color: c, text: t, capacity: cap});
  }
  var scene = pocket.kasane.createScene({
    build: function (tx, s) {
      tx.background(0x08131fff);
      T(tx, 10, 8, 220, 10, 0x7fd7ffff, 'POCKET BRIDGE / PC LINK');
      tx.rect({bounds: [8, 24, 232, 98], color: 0x14293cff});
      var r = {
        st: T(tx, 16, 31, 208, 10, 0xf5eedcff, s.st, 32),
        info: T(tx, 16, 49, 208, 10, 0x9fb6c8ff, s.info, 40),
        job: T(tx, 16, 66, 208, 10, 0x9fb6c8ff, s.job, 24),
        seen: T(tx, 16, 83, 208, 10, 0x8ef0c4ff, s.seen, 32)
      };
      T(tx, 10, 120, 220, 9, 0xf5bb69ff, 'ENTER SUBMIT   ESC HOME');
      return r;
    },
    patch: function (tx, r, s) { for (var k in r) r[k].setText(tx, s[k]); }
  });
  function show(f, t) { if (state[f] !== t) { state[f] = t; scene.invalidate(); } }
  var cap = pocket.capabilities.get('bridge.pc');
  console.log('BRIDGE_CAP supported=' + cap.supported + ' available=' + cap.available +
    ' maxFrameBytes=' + cap.limits.maxFrameBytes + ' maxPayloadBytes=' + cap.limits.maxPayloadBytes);
  var link = null, busy = false;
  function fail(where, e) {
    var c = (e && e.code) || '?';
    show('st', where + ' ' + c);
    console.log('BRIDGE_ERROR ' + where + ' ' + c + ' outcome=' + ((e && e.outcome) || 'none'));
  }
  pocket.bridge.connect({transport: 'usb', peerId: 'pocket-bridge-demo'}, {timeoutMs: 10000})
    .then(function (b) {
      link = b;
      show('st', 'LINKED ' + b.sessionId);
      console.log('BRIDGE_LINK ' + b.sessionId);
      b.onEvent('agent.job', function (e) {
        show('seen', 'EVENT ' + e.sequence + ' ' + e.payload.state);
        console.log('BRIDGE_EVENT ' + e.sequence + ' ' + e.payload.state);
      });
      return b.request('host.info');
    })
    .then(function (r) {
      show('info', 'HOST ' + r.host);
      console.log('BRIDGE_INFO ' + r.host + ' ' + r.system);
    }, function (e) { fail('CONNECT', e); });
  pocket.input.onAction(function (e) {
    if (e.action !== 'accept' || e.phase !== 'press' || !link || busy) return;
    busy = true;
    show('job', 'SUBMITTING...');
    link.request('agent.submit', {prompt: 'hello from the cardputer'}, {timeoutMs: 5000})
      .then(function (r) {
        busy = false;
        show('job', 'JOB ' + r.jobId);
        console.log('BRIDGE_JOB ' + r.jobId + ' ' + r.state);
      }, function (e) { busy = false; fail('SUBMIT', e); });
  });
  globalThis.frame = function () { scene.flush(state); };
  scene.flush(state);
  console.log('BRIDGE_READY');
})();
