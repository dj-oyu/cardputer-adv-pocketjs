// USB diagnostic b: another SD track with 16 KiB deliberately retained by JS.
(function () {
  const folder = 'sd:/KAKATO/KARA OK 2nd Edition';
  const view = pocket.kasane.mount({version: 1,
    slots: {elapsed: {type: 'text', capacity: 8},
            frames: {type: 'u32'}, starved: {type: 'u32'}, streamId: {type: 'u32'}},
    nodes: [
      {type: 'rect', bounds: [0, 0, 96, 24], color: 0x060c1aff},
      {type: 'text', bounds: [4, 4, 92, 16], text: {slot: 'elapsed'}, color: 0xe2f0ffff}
    ]}, {elapsed: '--------', frames: 0, starved: 0, streamId: 0});
  const source = pocket.audio.outputSource();
  let pressure = null;
  let bound = false;
  let playerHandle = null;
  let stateSubscription = null;
  let playStartMs = 0;
  let closed = false;

  async function chooseTrack() {
    let cursor = null;
    for (let page = 0; page < 8; page++) {
      const listed = await pocket.fs.list(folder,
        cursor ? {limit: 16, cursor: cursor} : {limit: 16});
      for (const entry of listed.entries) {
        if (entry.kind === 'file' && entry.accessible &&
            /\.mp3$/i.test(entry.name) &&
            !entry.name.startsWith('01 ') && !entry.name.startsWith('02 '))
          return entry.path;
      }
      cursor = listed.nextCursor;
      if (!cursor) break;
    }
    throw new Error('no alternate MP3 in the granted folder');
  }

  function fail(where, error) {
    console.log('KSN_OUTPUT_SOURCE ERROR ' + where + ' ' +
                ((error && error.code) || error) + ' ' +
                ((error && error.message) || ''));
    if (playerHandle && !closed) { closed = true; playerHandle.close(); }
  }
  async function prepare() {
    try {
      const root = await pocket.fs.requestFolder('sd');
      if (root !== 'sd:/') throw new Error('SD folder not granted');
      const track = await chooseTrack();
      console.log('KSN_OUTPUT_SOURCE TRACK ' + track);
      pressure = new Uint8Array(16384);
      for (let at = 0; at < pressure.length; at += 4096) pressure[at] = 0x5a;
      console.log('KSN_OUTPUT_SOURCE LOW_HEAP bytes=' + pressure.length);
      playerHandle = await pocket.audio.player.open({source: track});
      const info = playerHandle.info();
      console.log('KSN_OUTPUT_SOURCE OPEN codec=' + info.codec +
                  ' durationMs=' + info.durationMs);
      if (info.codec !== 'mp3') throw new Error('alternate track is not MP3');
      stateSubscription = playerHandle.onState(function (event) {
        console.log('KSN_OUTPUT_SOURCE STATE ' + event.state);
        if (event.state === 'error') fail('PLAYBACK', event.error);
      });
      await playerHandle.play();
      playStartMs = pocket.time.now();
      console.log('KSN_OUTPUT_SOURCE PLAY');
    } catch (error) { fail('PREPARE', error); }
  }
  globalThis.frame = function () {
    if (playStartMs && !closed && pocket.time.now() - playStartMs >= 46000) {
      closed = true;
      const status = playerHandle.status();
      console.log('KSN_OUTPUT_SOURCE FINAL positionMs=' + status.positionMs +
                  ' underruns=' + status.underruns +
                  ' pressureByte=' + pressure[0]);
      playerHandle.close();
      console.log('KSN_OUTPUT_SOURCE CLOSED');
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
    prepare();
  };
})();
