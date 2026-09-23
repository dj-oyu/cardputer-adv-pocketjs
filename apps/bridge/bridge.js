(function () {
  var view = pocket.kasane.mount('bridge');
  view.set({st: 'CONNECTING...', info: '', job: '', seen: ''});
  function show(f, t) { var slot = {}; slot[f] = t; view.set(slot); }
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
  globalThis.frame = function () {};
  console.log('BRIDGE_READY');
})();
