(function () {
  var P = {w: 1, h: 2, pos: 24, top: 25, left: 28, bg: 64, color: 96};
  function node(k, x, y, w, h, c, t) {
    var n = ui.createNode(k);
    ui.setProp(n, P.pos, 1); ui.setProp(n, P.left, x); ui.setProp(n, P.top, y);
    ui.setProp(n, P.w, w); ui.setProp(n, P.h, h);
    ui.setProp(n, k === 1 ? P.color : P.bg, c);
    if (t) ui.replaceText(n, t);
    ui.insertBefore(1, n, 0); return n;
  }
  var shown = {};
  function text(n, t) { if (shown[n] !== t) { shown[n] = t; ui.replaceText(n, t); } }
  ui.setProp(1, P.bg, 0x08131fff);
  node(1, 10, 8, 220, 10, 0x7fd7ffff, 'POCKET BRIDGE / PC LINK');
  node(0, 8, 24, 224, 74, 0x14293cff);
  var state = node(1, 16, 31, 208, 10, 0xf5eedcff, 'CONNECTING...');
  var info = node(1, 16, 49, 208, 10, 0x9fb6c8ff, '');
  var job = node(1, 16, 66, 208, 10, 0x9fb6c8ff, '');
  var seen = node(1, 16, 83, 208, 10, 0x8ef0c4ff, '');
  node(1, 10, 120, 220, 9, 0xf5bb69ff, 'ENTER SUBMIT   ESC HOME');

  var cap = pocket.capabilities.get('bridge.pc');
  console.log('BRIDGE_CAP supported=' + cap.supported + ' available=' + cap.available +
              ' maxFrameBytes=' + cap.limits.maxFrameBytes +
              ' maxPayloadBytes=' + cap.limits.maxPayloadBytes);
  var link = null, prev = 0, busy = false;
  function fail(where, e) {
    text(state, where + ' ' + ((e && e.code) || '?'));
    console.log('BRIDGE_ERROR ' + where + ' ' + ((e && e.code) || String(e)) +
                ' outcome=' + ((e && e.outcome) || 'none'));
  }
  pocket.bridge.connect({transport: 'usb', peerId: 'pocket-bridge-demo'},
                        {timeoutMs: 10000})
    .then(function (b) {
      link = b;
      text(state, 'LINKED ' + b.sessionId);
      console.log('BRIDGE_LINK ' + b.sessionId);
      b.onEvent('agent.job', function (e) {
        text(seen, 'EVENT ' + e.sequence + ' ' + e.payload.state);
        console.log('BRIDGE_EVENT ' + e.sequence + ' ' + e.payload.state);
      });
      return b.request('host.info');
    })
    .then(function (r) {
      text(info, 'HOST ' + r.host);
      console.log('BRIDGE_INFO ' + r.host + ' ' + r.system);
    }, function (e) { fail('CONNECT', e); });

  globalThis.frame = function (buttons) {
    if ((buttons & 0x4000) && !(prev & 0x4000) && link && !busy) {
      busy = true;
      text(job, 'SUBMITTING...');
      link.request('agent.submit', {prompt: 'hello from the cardputer'},
                   {timeoutMs: 5000})
        .then(function (r) {
          busy = false;
          text(job, 'JOB ' + r.jobId);
          console.log('BRIDGE_JOB ' + r.jobId + ' ' + r.state);
        }, function (e) { busy = false; fail('SUBMIT', e); });
    }
    prev = buttons;
  };
  console.log('BRIDGE_READY');
})();
