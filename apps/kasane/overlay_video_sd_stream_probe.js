// D6 diagnostic: one persistent SD KSV1 reader, the transient SD MP3 reader,
// and the native FLOWER backdrop. The full-app stage created the reserved file.
(() => {
  const path = 'sd:/__ksn_d6_av_stream_probe_20260928.ksv';
  const mp3 = 'sd:/KAKATO/KARA OK 2nd Edition/01 KAKATORO.mp3';
  const width = 32, height = 8, expectedBytes = 157216;
  const video = pocket.kasane.video;
  const resource = video.open(width, height);
  let stream = false, player = null;
  let playing = false;
  let requested = false, finishing = false, readyAt = 0;
  let selected = 0, lastStatusSecond = -1, lastState = '';

  pocket.kasane.replace(tx => {
    tx.background(0x000000ff);
    tx.image({resource, bounds: [2, 2, 66, 18], scale: 2});
    tx.rect({bounds: [42, 4, 63, 16], color: 0xffe08080});
    tx.text({bounds: [69, 4, 94, 17], text: 'D6', font: 'caption',
             color: 0xe7f4ffff});
  });
  function cleanup() {
    if (player) {
      const status = player.status();
      console.log('D6_AV MP3_STOP state=' + status.state +
                  ' positionMs=' + status.positionMs +
                  ' underruns=' + status.underruns);
      player.close(); player = null;
    }
    if (stream) {
      const acked = video.streamStop();
      stream = false;
      console.log('D6_AV STOP ack=' + acked);
    }
    console.log('D6_AV FILE_RETAINED');
  }
  async function finish(reason) {
    if (finishing) return;
    finishing = true;
    try {
      cleanup();
      console.log('D6_AV DONE reason=' + reason + ' selected=' + selected);
    } catch (error) {
      console.log('D6_AV CLEANUP_ERROR code=' + ((error && error.code) || error));
    }
  }
  async function prepare() {
    try {
      console.log('D6_AV GRANT_REQUEST');
      if (await pocket.fs.requestFolder('sd') !== 'sd:/')
        throw new Error('folder grant declined');
      console.log('D6_AV GRANTED');
      const entry = await pocket.fs.stat(path);
      if (entry.sizeBytes !== expectedBytes)
        throw new Error('staged KSV size mismatch');
      console.log('D6_AV STAGED bytes=' + entry.sizeBytes);
      video.streamStart(path); stream = true;
      console.log('D6_AV STREAM_START');
      player = await pocket.audio.player.open({source: mp3});
      const info = player.info();
      console.log('D6_AV MP3_OPEN codec=' + info.codec +
                  ' durationMs=' + info.durationMs);
      if (info.codec !== 'mp3') throw new Error('wrong audio codec');
      await player.play();
      readyAt = pocket.time.now(); playing = true;
      console.log('D6_AV MP3_PLAY');
    } catch (error) {
      console.log('D6_AV ERROR prepare=' + ((error && error.code) || error));
      await finish('prepare-error');
    }
  }
  pocket.overlay.onKey(event => {
    if (event.action !== 'accept' || requested) return;
    requested = true;
    prepare();
  });
  globalThis.frame = function () {
    if (!stream || !playing || finishing || !readyAt) return;
    try {
      const elapsed = pocket.time.now() - readyAt;
      const state = video.streamState();
      if (state !== lastState) {
        console.log('D6_AV STREAM_STATE ' + state);
        lastState = state;
      }
      if (state === 'error') { finish('stream-error'); return; }
      // The decoder's presented position is the diagnostic video clock.
      // A paused or ended player keeps its final position here.
      const status = player.status();
      if (status.state === 'error') { finish('mp3-error'); return; }
      if (video.streamPoll(status.positionMs * 1000)) {
        ++selected;
        if (selected === 1 || selected % 30 === 0)
          console.log('D6_AV SELECT n=' + selected);
      }
      const second = Math.floor(elapsed / 1000);
      if (second !== lastStatusSecond) {
        console.log('D6_AV MP3_STATUS second=' + second +
                    ' state=' + status.state +
                    ' positionMs=' + status.positionMs +
                    ' underruns=' + status.underruns);
        lastStatusSecond = second;
      }
      if (state === 'end' && elapsed >= 10500) {
        console.log('D6_AV EOF');
        finish('end');
      } else if (elapsed >= 16000) finish('timeout');
    } catch (error) {
      console.log('D6_AV ERROR frame=' + ((error && error.code) || error));
      finish('frame-error');
    }
  };
})();
