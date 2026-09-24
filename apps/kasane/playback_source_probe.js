// USB diagnostic c: current-player source drives a generic mount's visibility.
(function () {
  const view = pocket.kasane.mount({version: 1,
    slots: {state: {type: 'u16'}, positionMs: {type: 'u32'},
            durationMs: {type: 'u32'}, underruns: {type: 'u32'},
            playing: {type: 'bool'}},
    nodes: [
      {type: 'rect', bounds: [0, 0, 96, 48], color: 0x060c1aff},
      {type: 'text', bounds: [4, 4, 92, 16], text: 'PLAY',
       visible: {slot: 'playing'}, color: 0xe2f0ffff}
    ]}, {state: 0, positionMs: 0, durationMs: 0, underruns: 0,
         playing: false});
  const source = pocket.audio.playbackSource();
  let bound = false;
  let playerHandle = null;
  let subscription = null;
  let openedMs = 0;
  let playStartMs = 0;
  let pauseStartMs = 0;
  let phase = 'prepare';
  let closed = false;

  function fail(where, error) {
    console.log('KSN_PLAYBACK_SOURCE ERROR ' + where + ' ' +
                ((error && error.code) || error));
    if (playerHandle && !closed) { closed = true; playerHandle.close(); }
  }
  async function prepare() {
    try {
      const root = await pocket.fs.requestFolder('sd');
      if (root !== 'sd:/') throw new Error('SD folder not granted');
      playerHandle = await pocket.audio.player.open({
        source: 'sd:/KAKATO/KARA OK 2nd Edition/02 インザハウス.mp3'
      });
      subscription = playerHandle.onState(function (event) {
        console.log('KSN_PLAYBACK_SOURCE STATE ' + event.state);
        if (event.state === 'error') fail('PLAYBACK', event.error);
      });
      openedMs = pocket.time.now();
      phase = 'ready';
      console.log('KSN_PLAYBACK_SOURCE OPEN');
    } catch (error) { fail('PREPARE', error); }
  }
  globalThis.frame = function () {
    const now = pocket.time.now();
    if (phase === 'ready' && now - openedMs >= 3000) {
      phase = 'starting';
      playerHandle.play().then(function () {
        playStartMs = pocket.time.now();
        phase = 'playing';
        console.log('KSN_PLAYBACK_SOURCE PLAY');
      }, function (error) { fail('PLAY', error); });
    } else if (phase === 'playing' && now - playStartMs >= 7000) {
      phase = 'pausing';
      playerHandle.pause().then(function () {
        pauseStartMs = pocket.time.now();
        phase = 'paused';
        console.log('KSN_PLAYBACK_SOURCE PAUSED positionMs=' +
                    playerHandle.status().positionMs);
      }, function (error) { fail('PAUSE', error); });
    } else if (phase === 'paused' && now - pauseStartMs >= 3000) {
      phase = 'resuming';
      playerHandle.play().then(function () {
        phase = 'resumed';
        console.log('KSN_PLAYBACK_SOURCE RESUMED positionMs=' +
                    playerHandle.status().positionMs);
      }, function (error) { fail('RESUME', error); });
    } else if (phase === 'resumed' && now - playStartMs >= 23000) {
      phase = 'done';
      const status = playerHandle.status();
      console.log('KSN_PLAYBACK_SOURCE FINAL positionMs=' + status.positionMs +
                  ' underruns=' + status.underruns);
      closed = true;
      playerHandle.close();
      console.log('KSN_PLAYBACK_SOURCE CLOSED');
    }
    if (bound) return;
    try { view.bind(source, {state: 0, positionMs: 1, durationMs: 2,
                             underruns: 3, playing: 4}); }
    catch (error) {
      if (error.code === 'BUSY') return;
      fail('BIND', error);
      throw error;
    }
    bound = true;
    console.log('KSN_PLAYBACK_SOURCE BOUND');
    prepare();
  };
})();
