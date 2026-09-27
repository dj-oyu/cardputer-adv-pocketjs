// USB diagnostic i. A seekable WAV gives music a stable, known-duration bar.
// Never overwrite a pre-existing app:/ file; remove only the file we create.
(function () {
  const path = 'app:/__ksn_music_bar_probe_20260924.wav';
  const blocks = 11;
  const blockBytes = 2048;
  const perBlock = 1 + (blockBytes - 4) * 2;
  const view = pocket.kasane.mount('music');
  view.set({title: 'BAR TEST', message: ''});
  let started = false;
  let readyAt = 0;
  let playAt = 0;
  let transitionStarted = false;
  let ownedFile = false;
  let closed = false;
  let player = null;
  let file = null;

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
  function header() {
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
  async function finish() {
    if (closed) return;
    closed = true;
    if (player) { player.close(); player = null; }
    if (file) { file.close(); file = null; }
    if (ownedFile) {
      await pocket.fs.remove(path);
      ownedFile = false;
      console.log('KSN_MUSIC_BAR REMOVED');
    }
  }
  function fail(where, error) {
    console.log('KSN_MUSIC_BAR ERROR ' + where + ' ' +
                ((error && error.code) || error));
    finish().catch(function (cleanup) {
      console.log('KSN_MUSIC_BAR ERROR cleanup ' +
                  ((cleanup && cleanup.code) || cleanup));
    });
  }
  async function prepare() {
    try {
      try {
        await pocket.fs.stat(path);
        throw new Error('the diagnostic file already exists');
      } catch (error) {
        if (error.code !== 'NOT_FOUND') throw error;
      }
      file = await pocket.fs.open(path, {mode: 'create'});
      ownedFile = true;
      await file.write(header());
      const half = new Uint8Array(1024);
      for (let i = 0; i < blocks * 2; i++) await file.write(half);
      await file.commit();
      file = null;
      console.log('KSN_MUSIC_BAR CREATED bytes=' + (48 + blocks * blockBytes));
      player = await pocket.audio.player.open({source: path});
      const info = player.info();
      if (info.codec !== 'wav/ima-adpcm' || !info.seekable ||
          info.durationMs < 1800 || info.durationMs > 1900)
        throw new Error('unexpected WAV information');
      await player.seek(1120);
      const status = player.status();
      if (status.state !== 'ready' || status.positionMs !== 1120)
        throw new Error('seek did not hold the ready position');
      view.bind('playback');
      readyAt = pocket.time.now();
      console.log('KSN_MUSIC_BAR READY positionMs=' + status.positionMs +
                  ' durationMs=' + info.durationMs);
    } catch (error) { fail('PREPARE', error); }
  }
  globalThis.frame = function () {
    if (!started) { started = true; prepare(); }
    if (readyAt && !closed && !transitionStarted &&
        pocket.time.now() - readyAt > 13000) {
      transitionStarted = true;
      player.seek(0).then(function () { return player.play(); })
        .then(function () {
          playAt = pocket.time.now();
          console.log('KSN_MUSIC_BAR PLAYING');
          return pocket.time.sleep(120);
        })
        .then(function () {
          const elapsed = pocket.time.now() - playAt;
          const state = player.status().state;
          console.log('KSN_MUSIC_BAR SLEEP_DONE elapsedMs=' + elapsed +
                      ' state=' + state);
          if (elapsed > 500 || state !== 'playing')
            throw new Error('guest was not serviced during playback');
          return player.pause();
        })
        .then(function () { return player.seek(1120); })
        .then(function () {
          const status = player.status();
          if (status.state !== 'paused' || status.positionMs !== 1120)
            throw new Error('pause/seek did not hold the position');
          console.log('KSN_MUSIC_BAR PAUSED positionMs=' + status.positionMs);
        }).catch(function (error) { fail('PAUSE_SEEK', error); });
    }
    if (readyAt && !closed && pocket.time.now() - readyAt > 38000)
      finish().catch(function (error) { fail('REMOVE', error); });
  };
})();
