// USB diagnostic j. A short seekable WAV lives only for this test in app:/.
// 11 silent IMA blocks fit below app:/'s 24,576-byte per-file limit.
(function () {
  const path = 'app:/__ksn_seek_probe_20260924.wav';
  const blocks = 11;
  const blockBytes = 2048;
  const perBlock = 1 + (blockBytes - 4) * 2;
  const view = pocket.kasane.mount({version: 1,
    slots: {elapsed: {type: 'text', capacity: 8},
            frames: {type: 'u32'}, starved: {type: 'u32'}, streamId: {type: 'u32'}},
    nodes: [
      {type: 'rect', bounds: [0, 0, 96, 24], color: 0x060c1aff},
      {type: 'text', bounds: [4, 4, 92, 16], text: {slot: 'elapsed'}, color: 0xe2f0ffff}
    ]}, {elapsed: '--------', frames: 0, starved: 0, streamId: 0});
  const source = pocket.audio.outputSource();
  let bound = false;
  let playerHandle = null;
  let stateSubscription = null;
  let fileHandle = null;
  let ownedFile = false;
  let playStartMs = 0;
  let seekStartMs = 0;
  let phase = 'prepare';
  let prepareStep = 'stat';
  let closed = false;

  function word(bytes, at, value) {
    bytes[at] = value & 255;
    bytes[at + 1] = (value >>> 8) & 255;
  }
  function dword(bytes, at, value) {
    word(bytes, at, value);
    word(bytes, at + 2, value >>> 16);
  }
  function tag(bytes, at, value) {
    for (let i = 0; i < 4; i++) bytes[at + i] = value.charCodeAt(i);
  }
  function wavHeader() {
    const dataBytes = blocks * blockBytes;
    const bytes = new Uint8Array(48);
    tag(bytes, 0, 'RIFF'); dword(bytes, 4, dataBytes + 40);
    tag(bytes, 8, 'WAVE'); tag(bytes, 12, 'fmt ');
    dword(bytes, 16, 20); word(bytes, 20, 0x11); word(bytes, 22, 1);
    dword(bytes, 24, 24000);
    dword(bytes, 28, Math.floor(24000 * blockBytes / perBlock));
    word(bytes, 32, blockBytes); word(bytes, 34, 4);
    word(bytes, 36, 2); word(bytes, 38, perBlock);
    tag(bytes, 40, 'data'); dword(bytes, 44, dataBytes);
    return bytes;
  }
  function fail(where, error) {
    console.log('KSN_OUTPUT_SOURCE ERROR ' + where + ' ' +
                ((error && error.code) || error) + ' step=' + prepareStep +
                ' message=' + ((error && error.message) || ''));
    if (playerHandle && !closed) { closed = true; playerHandle.close(); }
    if (fileHandle) { fileHandle.close(); fileHandle = null; }
    if (ownedFile) pocket.fs.remove(path).catch(function () {});
  }
  function finish() {
    if (closed) return;
    closed = true;
    const status = playerHandle.status();
    console.log('KSN_OUTPUT_SOURCE FINAL positionMs=' + status.positionMs +
                ' underruns=' + status.underruns);
    playerHandle.close();
    pocket.fs.remove(path).then(function () {
      ownedFile = false;
      console.log('KSN_OUTPUT_SOURCE REMOVED');
      console.log('KSN_OUTPUT_SOURCE CLOSED');
    }, function (error) { fail('REMOVE', error); });
  }
  async function prepare() {
    try {
      try {
        await pocket.fs.stat(path);
        throw new Error('the diagnostic file already exists');
      } catch (error) {
        if (error.code !== 'NOT_FOUND') throw error;
      }
      prepareStep = 'open';
      fileHandle = await pocket.fs.open(path, {mode: 'create'});
      ownedFile = true;
      prepareStep = 'write header';
      await fileHandle.write(wavHeader());
      const halfBlock = new Uint8Array(1024);
      prepareStep = 'write blocks';
      for (let i = 0; i < blocks * 2; i++) await fileHandle.write(halfBlock);
      prepareStep = 'commit';
      await fileHandle.commit();
      fileHandle = null;
      console.log('KSN_OUTPUT_SOURCE CREATED bytes=' + (48 + blocks * blockBytes));
      prepareStep = 'player open';
      const player = await pocket.audio.player.open({source: path});
      playerHandle = player;
      const info = player.info();
      console.log('KSN_OUTPUT_SOURCE OPEN codec=' + info.codec +
                  ' durationMs=' + info.durationMs + ' seekable=' + info.seekable);
      if (info.codec !== 'wav/ima-adpcm' || info.seekable !== true ||
          info.durationMs < 1800 || info.durationMs > 1900)
        throw new Error('unexpected seekable WAV information');
      stateSubscription = player.onState(function (event) {
        console.log('KSN_OUTPUT_SOURCE STATE ' + event.state);
        if (event.state === 'ended') finish();
        else if (event.state === 'error') fail('PLAYBACK', event.error);
      });
      prepareStep = 'play';
      await player.play();
      playStartMs = pocket.time.now();
      phase = 'playing';
      console.log('KSN_OUTPUT_SOURCE PLAY');
    } catch (error) { fail('PREPARE', error); }
  }
  globalThis.frame = function () {
    if (playStartMs && !closed) {
      const now = pocket.time.now();
      if (now - playStartMs > 6000) { fail('TIMEOUT', now - playStartMs); return; }
      if (phase === 'playing' && now - playStartMs >= 200) {
        phase = 'seeking';
        seekStartMs = now;
        console.log('KSN_OUTPUT_SOURCE SEEK_REQUEST positionMs=' +
                    playerHandle.status().positionMs + ' targetMs=1120');
        playerHandle.seek(1120).then(function () {
          phase = 'seeked';
          console.log('KSN_OUTPUT_SOURCE SEEK_ACCEPT positionMs=' +
                      playerHandle.status().positionMs +
                      ' latencyMs=' + (pocket.time.now() - seekStartMs));
        }, function (error) { fail('SEEK', error); });
      } else if (phase === 'seeked' && playerHandle.status().positionMs >= 1200) {
        phase = 'progress';
        console.log('KSN_OUTPUT_SOURCE SEEK_PROGRESS positionMs=' +
                    playerHandle.status().positionMs +
                    ' latencyMs=' + (now - seekStartMs));
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
    prepare();
  };
})();
