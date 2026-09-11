// VM_PROBE L0 contention conditions (spec sec.5), evaluated after a workload.
// globalThis.VMC is the mask: 1 UI, 2 audio, 4 Wi-Fi. Reasons: README.md.
(function () {
  var m = globalThis.VMC | 0, f = globalThis.frame, log = console.log;
  var P = pocket, n = 0, t = null;
  if (m & 1) {
    var s = P.ui.screen({ background: 0x101820ff });
    s.rect({ x: 0, y: 96, width: 240, height: 39, color: 0x203048ff });
    t = s.text({ x: 6, y: 108, width: 228, height: 12, font: 'small',
                 color: 0xd0e0ffff, text: 'VMPROBE 0' });
    P.ui.push(s);
    log('VMCOND ui on');
  }
  if (m & 2) {
    var beeps = 0, beep = function () {
      P.audio.tone({ frequencyHz: 440, durationMs: 1000, gain: 0.2 }).then(
        function () { if (++beeps % 5 === 0) log('VMCOND tone ' + beeps); beep(); },
        function (e) { log('VMCOND tone ' + e.code); });
    };
    beep();
  }
  if (m & 4) {
    var URL = '', got = 0, pulls;
    var get = function (w) {
      var t0 = P.time.now(), st = 0;
      P.net.http.request({ wifi: w, url: URL, method: 'GET' }).then(
        function (r) {
          st = r.status;
          pulls = function (b) {
            return r.read(1024).then(function (c) {
              if (c !== null) return pulls(b + c.length);
              r.close();
              log('VMCOND http ' + st + ' ' + b + ' ' +
                  Math.round(P.time.now() - t0) + 'ms n=' + (++got));
            });
          };
          return pulls(0);
        },
        function (e) { log('VMCOND http ' + e.code + ' ' + (e.message || '')); })
        .then(function () { return P.time.sleep(3000); })
        .then(function () { get(w); })
        // An unhandled rejection ends the session (guest.c's drain_jobs
        // reports one as a frame error), so the chain must not leave one.
        .catch(function (e) { log('VMCOND req ' + (e && e.code || String(e))); });
    };
    P.net.wifi.acquire({ profileId: 'default' }).then(
      function (w) {
        var ip = w.status().address || '';
        // The LAN gateway, from this device's own address: plain HTTP to a
        // private address is what the firmware allows without a clock, and it
        // needs no server on the capture host. HTTPS was measured first and
        // does not fit -- see README.md.
        URL = 'http://' + ip.replace(/\.\d+$/, '.1') + '/';
        log('VMCOND wifi ' + w.status().state + ' ' + ip + ' -> ' + URL);
        get(w);
      },
      function (e) { log('VMCOND wifi ' + e.code + ' ' + (e.message || '')); });
  }
  globalThis.frame = function (a, b, c, d) {
    if (t) t.setText('VMPROBE ' + (n = (n + 1) & 1023));
    return f(a, b, c, d);
  };
})();
