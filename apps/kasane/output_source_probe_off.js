// USB diagnostic u: same SD playback and descriptor as v, without creating
// audio.outputSource() or installing the audio-task observer.
(function () {
  pocket.kasane.mount({version: 1,
    slots: {elapsed: {type: 'text', capacity: 8}},
    nodes: [
      {type: 'rect', bounds: [0, 0, 96, 24], color: 0x060c1aff},
      {type: 'text', bounds: [4, 4, 92, 16], text: {slot: 'elapsed'}, color: 0xe2f0ffff}
    ]}, {elapsed: '--------'});
  let playerHandle = null;
  let stateSubscription = null;
  let playStartMs = 0;
  let closed = false;
  let started = false;
  globalThis.frame = function () {
    if (playStartMs && !closed && pocket.time.now() - playStartMs >= 45000) {
      closed = true;
      const status = playerHandle.status();
      console.log('KSN_OUTPUT_SOURCE FINAL positionMs=' + status.positionMs +
                  ' underruns=' + status.underruns);
      playerHandle.close();
      console.log('KSN_OUTPUT_SOURCE CLOSED');
    }
    if (started) return;
    started = true;
    console.log('KSN_OUTPUT_SOURCE BOUND');
    pocket.fs.requestFolder('sd').then(function (root) {
      if (root !== 'sd:/') throw new Error('SD folder not granted');
      return pocket.audio.player.open({
        source: 'sd:/KAKATO/KARA OK 2nd Edition/02 インザハウス.mp3'
      });
    }).then(function (player) {
      playerHandle = player;
      console.log('KSN_OUTPUT_SOURCE OPEN');
      stateSubscription = player.onState(function (event) {
        console.log('KSN_OUTPUT_SOURCE STATE ' + event.state);
      });
      player.play().then(function () {
        playStartMs = pocket.time.now();
        console.log('KSN_OUTPUT_SOURCE PLAY');
      }, function (error) {
        console.log('KSN_OUTPUT_SOURCE ERROR PLAY ' + error);
      });
    }, function (error) {
      console.log('KSN_OUTPUT_SOURCE ERROR OPEN ' + error);
    });
  };
})();
