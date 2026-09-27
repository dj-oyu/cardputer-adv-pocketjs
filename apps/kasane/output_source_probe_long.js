// USB diagnostic y. Play the entire MP3 with two timed pause/resume cycles.
// Kasane consumes the native output source; JS never writes elapsed text.
(function () {
  const view = pocket.kasane.mount({version: 1,
    slots: {elapsed: {type: 'text', capacity: 8},
            frames: {type: 'u32'}, starved: {type: 'u32'}, streamId: {type: 'u32'}},
    nodes: [
      {type: 'rect', bounds: [0, 0, 96, 24], color: 0x060c1aff},
      {type: 'text', bounds: [4, 4, 92, 16], text: {slot: 'elapsed'}, color: 0xe2f0ffff}
    ]}, {elapsed: '--------', frames: 0, starved: 0, streamId: 0});
  const source = pocket.audio.outputSource({sampleMs: 33});
  const pauseAtMs = [45000, 140000];
  let bound = false;
  let playerHandle = null;
  let stateSubscription = null;
  let playStartMs = 0;
  let nextTickMs = 30000;
  let cycle = 0;
  let phase = 'playing';
  let pauseWallMs = 0;
  let pausePositionMs = 0;
  let resumeWallMs = 0;
  let closed = false;
  function fail(where, error) {
    console.log('KSN_OUTPUT_SOURCE ERROR ' + where + ' ' +
                ((error && error.code) || error));
    if (playerHandle && !closed) { closed = true; playerHandle.close(); }
  }
  function finish() {
    if (closed) return;
    closed = true;
    const status = playerHandle.status();
    if (cycle !== 2 || phase !== 'playing')
      console.log('KSN_OUTPUT_SOURCE ERROR MISSING_CYCLES cycle=' + cycle + ' phase=' + phase);
    console.log('KSN_OUTPUT_SOURCE FINAL positionMs=' + status.positionMs +
                ' underruns=' + status.underruns + ' cycles=' + cycle);
    playerHandle.close();
    console.log('KSN_OUTPUT_SOURCE CLOSED');
  }
  globalThis.frame = function () {
    if (playStartMs && !closed) {
      const now = pocket.time.now();
      const elapsed = now - playStartMs;
      if (elapsed >= nextTickMs) {
        console.log('KSN_OUTPUT_SOURCE TICK wallMs=' + elapsed +
                    ' positionMs=' + playerHandle.status().positionMs);
        nextTickMs += 30000;
      }
      if (elapsed > 290000) { fail('TIMEOUT', elapsed); return; }
      if (phase === 'playing' && cycle < 2 && elapsed >= pauseAtMs[cycle]) {
        phase = 'pause-pending';
        console.log('KSN_OUTPUT_SOURCE PAUSE_REQUEST cycle=' + cycle +
                    ' wallMs=' + elapsed);
        playerHandle.pause().then(function () {
          pauseWallMs = pocket.time.now();
          pausePositionMs = playerHandle.status().positionMs;
          phase = 'paused';
          console.log('KSN_OUTPUT_SOURCE PAUSED cycle=' + cycle +
                      ' positionMs=' + pausePositionMs);
        }, function (error) { fail('PAUSE', error); });
      } else if (phase === 'paused' && now - pauseWallMs >= 3000) {
        phase = 'resume-pending';
        resumeWallMs = now;
        console.log('KSN_OUTPUT_SOURCE RESUME_REQUEST cycle=' + cycle +
                    ' positionMs=' + playerHandle.status().positionMs);
        playerHandle.play().then(function () {
          phase = 'resuming';
          console.log('KSN_OUTPUT_SOURCE RESUME_ACCEPT cycle=' + cycle +
                      ' latencyMs=' + (pocket.time.now() - resumeWallMs));
        }, function (error) { fail('RESUME', error); });
      } else if (phase === 'resuming' &&
                 playerHandle.status().positionMs >= pausePositionMs + 50) {
        console.log('KSN_OUTPUT_SOURCE RESUME_PROGRESS cycle=' + cycle +
                    ' latencyMs=' + (now - resumeWallMs));
        cycle++;
        phase = 'playing';
      }
    }
    if (bound) return;
    try { view.bind(source, {elapsed: 0, frames: 1, starved: 2, streamId: 3}); }
    catch (error) {
      if (error.code === 'BUSY') return;
      fail('BIND', error);
      throw error;
    }
    bound = true;
    console.log('KSN_OUTPUT_SOURCE BOUND');
    pocket.fs.requestFolder('sd').then(function (root) {
      if (root !== 'sd:/') throw new Error('SD folder not granted');
      return pocket.audio.player.open({
        source: 'sd:/KAKATO/KARA OK 2nd Edition/02 インザハウス.mp3'
      });
    }).then(function (player) {
      playerHandle = player;
      const info = player.info();
      console.log('KSN_OUTPUT_SOURCE OPEN durationMs=' + info.durationMs +
                  ' seekable=' + info.seekable);
      if (info.seekable !== false) { fail('MP3_SEEKABLE', info.seekable); return; }
      stateSubscription = player.onState(function (event) {
        console.log('KSN_OUTPUT_SOURCE STATE ' + event.state);
        if (event.state === 'ended') finish();
        else if (event.state === 'error') fail('PLAYBACK', event.error);
      });
      player.seek(1000).then(function () {
        fail('MP3_SEEK_ACCEPTED', 'unexpected success');
      }, function (error) {
        if (!error || error.code !== 'NOT_AVAILABLE') {
          fail('MP3_SEEK_REJECT', error);
          return;
        }
        console.log('KSN_OUTPUT_SOURCE SEEK_UNSUPPORTED code=' + error.code);
        player.play().then(function () {
          playStartMs = pocket.time.now();
          console.log('KSN_OUTPUT_SOURCE PLAY');
        }, function (reason) { fail('PLAY', reason); });
      });
    }, function (error) { fail('OPEN', error); });
  };
})();
