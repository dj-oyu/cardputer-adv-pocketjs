// Which of the three shrinks the largest free block? README.md says why this
// question comes before the shortfall. No ui.*: the guest heap is measured too.
//
// Three jobs, each run N times, each isolating ONE component. Whichever job's
// `after` marks fall monotonically is the one that fragments; a job whose marks
// return to where they started is exonerated. Reading them together is the whole
// design -- the previous run cycled all three at once and could only say that
// something drifted.
(function () {
  var URL = 'http://192.168.1.17:8000/tone.pok';
  var SRC = 'assets:/tone.pok';
  var N = 4;                     // enough for a trend, not just a step
  var KINDS = ['radio', 'player', 'conn'];

  function M(t) {
    var m = pocket.device.metrics();
    console.log('FIT M ' + t + ' free=' + m.heapFreeBytes +
                ' largest=' + m.largestFreeBlockBytes +
                ' js=' + Math.round(m.jsHeapBytes));
  }
  M('boot');

  var ki = 0, it = 0, st = 0, w = 0;
  var lease = null, p = null, ready = false;

  function fail(k) {
    return function (e) {
      var c = (e && e.code) ? e.code : e;
      console.log('FIT FAIL ' + k + ' ' + c + ' :: ' + ((e && e.message) || ''));
      // A job that fails still has to be closed down and still has to report
      // where it left the heap, because a failed open that leaks is exactly the
      // thing being hunted.
      st = 90; w = 0;
    };
  }
  function shut() {                       // idempotent, used by every exit path
    if (p) { p.close(); p = null; }
    if (lease) { lease.close(); lease = null; }
    ready = false;
  }
  function next() {
    it++;
    if (it >= N) { ki++; it = 0; }
    st = 0; w = 0;
    if (ki >= KINDS.length) { console.log('FIT END'); st = 99; }
  }
  function tag(s) { return KINDS[ki] + '.' + it + '.' + s; }

  function acquire(then) {
    pocket.net.wifi.acquire({ profileId: 'default' }).then(function (l) {
      lease = l; then();
    }, fail('acquire'));
  }
  function open(source, then) {
    pocket.audio.player.open({ source: source }).then(function (h) {
      p = h; ready = true; then();
    }, fail('open'));
  }

  globalThis.frame = function () {
    if (st === 99) return;
    w++;
    var kind = KINDS[ki];

    if (st === 90) {                      // a failed job, unwound the same way
      if (w < 30) return;
      shut();
      if (w < 150) return;
      M(tag('after'));
      next();
      return;
    }

    if (kind === 'radio') {               // the radio, alone
      if (st === 0) {
        M(tag('before')); st = 1; w = 0;
        acquire(function () { M(tag('up')); st = 2; w = 0; });
      } else if (st === 2 && w > 30) {
        shut(); st = 3; w = 0;
      } else if (st === 3 && w > 120) {   // lwIP keeps ~4.8 KiB; the rest returns
        M(tag('after')); next();
      }
    } else if (kind === 'player') {       // the player, alone, from flash
      if (st === 0) {
        M(tag('before')); st = 1; w = 0;
        open(SRC, function () {
          p.play().then(function () { M(tag('playing')); st = 2; w = 0; },
                        fail('play'));
        });
      } else if (st === 2 && w > 60) {    // not the whole clip: this is a heap
        shut(); st = 3; w = 0;            // question, not an audio one
      } else if (st === 3 && w > 60) {
        M(tag('after')); next();
      }
    } else {                              // the connection, alone: no play()
      if (st === 0) {
        M(tag('before')); st = 1; w = 0;
        acquire(function () {
          M(tag('linked')); st = 2; w = 0;
          // open() alone starts the receive task, the http client and the
          // packet ring. play() is deliberately never called, so whatever this
          // job costs is the CONNECTION and not the decoder.
          open(URL, function () { M(tag('open')); st = 3; w = 0; });
        });
      } else if (st === 3 && w > 30) {
        if (p) { p.close(); p = null; }
        st = 4; w = 0;
      } else if (st === 4 && w > 30) {
        M(tag('closed'));                 // player gone, lease still up
        if (lease) { lease.close(); lease = null; }
        st = 5; w = 0;
      } else if (st === 5 && w > 120) {
        M(tag('after')); next();
      }
    }
  };
})();
