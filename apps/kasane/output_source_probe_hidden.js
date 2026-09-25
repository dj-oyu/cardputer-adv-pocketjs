// USB diagnostic w. Hide the subscribed text while the real audio task keeps
// publishing, then reveal it without JS ever setting the elapsed-text slot.
(function () {
  const view = pocket.kasane.mount({version: 1,
    slots: {elapsed: {type: 'text', capacity: 8}, show: {type: 'bool'},
            frames: {type: 'u32'}, starved: {type: 'u32'}, streamId: {type: 'u32'}},
    nodes: [
      {type: 'rect', bounds: [0, 0, 96, 24], color: 0x060c1aff},
      {type: 'text', bounds: [4, 4, 92, 16], text: {slot: 'elapsed'},
       visible: {slot: 'show'}, color: 0xe2f0ffff}
    ]}, {elapsed: '--------', show: true, frames: 0, starved: 0, streamId: 0});
  const source = pocket.audio.outputSource();
  let bound = false;
  let playerHandle = null;
  let stateSubscription = null;
  let playStartMs = 0;
  let hidden = false;
  let shown = false;
  let closed = false;
  globalThis.frame = function () {
    if (playStartMs && !closed) {
      const elapsedMs = pocket.time.now() - playStartMs;
      if (!hidden && elapsedMs >= 8000) {
        view.set({show: false});
        hidden = true;
        console.log('KSN_OUTPUT_SOURCE HIDDEN');
      }
      if (hidden && !shown && elapsedMs >= 28000) {
        view.set({show: true});
        shown = true;
        console.log('KSN_OUTPUT_SOURCE SHOWN');
      }
      if (elapsedMs >= 45000) {
        closed = true;
        const status = playerHandle.status();
        console.log('KSN_OUTPUT_SOURCE FINAL positionMs=' + status.positionMs +
                    ' underruns=' + status.underruns);
        playerHandle.close();
        console.log('KSN_OUTPUT_SOURCE CLOSED');
      }
    }
    if (bound) return;
    try { view.bind(source, {elapsed: 0, frames: 1, starved: 2, streamId: 3}); }
    catch (error) {
      if (error.code === 'BUSY') return;
      console.log('KSN_OUTPUT_SOURCE ERROR ' + error);
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
