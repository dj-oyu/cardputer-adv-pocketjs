// D5 SD stream diagnostic. Choose a grant in the picker, then create and
// remove only the exclusive test file below. Never replace an existing file.
(() => {
  const path = 'sd:/__ksn_d5_stream_probe_20260928.ksv';
  const view = pocket.kasane;
  const video = view.video;
  const width = 32, height = 8, frames = 60;
  const resource = video.open(width, height);
  let file = null, owned = false, stream = false, started = false;
  let readyAt = 0, selected = 0, paused = false, resumed = false;
  let finishing = false;
  view.replace(tx => {
    tx.background(0x061322ff);
    tx.text({bounds: [8, 2, 232, 18], text: 'D5 SD RGB565 STREAM',
             font: 'caption', color: 0xe7f4ffff});
    tx.image({resource, bounds: [88, 40, 152, 56], scale: 2});
    tx.text({bounds: [8, 104, 232, 132], text: 'GRANTED SD / KSV1',
             font: 'caption', color: 0x80d9e8ff});
  });
  function u16(data, at, value) {
    data[at] = value & 255; data[at + 1] = (value >>> 8) & 255;
  }
  function u32(data, at, value) {
    u16(data, at, value); u16(data, at + 2, value >>> 16);
  }
  function header() {
    const data = new Uint8Array(16);
    data.set([75, 83, 86, 49], 0); // KSV1
    u16(data, 4, width); u16(data, 6, height);
    u32(data, 8, width * height * 2);
    return data;
  }
  function framePacket(i) {
    const frameBytes = width * height * 2;
    const data = new Uint8Array(12 + frameBytes);
    u32(data, 0, i * 33333); u32(data, 4, 0);
    u32(data, 8, frameBytes);
    for (let j = 0; j < width * height; ++j) {
      const x = j % width;
      const color = x >= (i % width) && x < (i % width) + 4
        ? 0xffe0 : (((i & 31) << 11) | 0x03ff);
      u16(data, 12 + 2 * j, color);
    }
    return data;
  }
  async function cleanup() {
    if (file) { file.close(); file = null; }
    if (stream) {
      const acked = video.streamStop();
      stream = false;
      console.log('D5_STREAM STOP ack=' + acked);
    }
    if (!owned) return;
    for (let attempt = 0; attempt < 40; ++attempt) {
      try {
        await pocket.fs.remove(path);
        owned = false;
        console.log('D5_STREAM REMOVED');
        return;
      } catch (error) {
        if (error.code !== 'BUSY' || attempt === 39) throw error;
        await pocket.time.sleep(50);
      }
    }
  }
  async function finish(reason) {
    if (finishing) return;
    finishing = true;
    try {
      await cleanup();
      console.log('D5_STREAM DONE reason=' + reason + ' selected=' + selected);
    } catch (error) {
      console.log('D5_STREAM CLEANUP_ERROR ' + ((error && error.code) || error));
    }
  }
  async function prepare() {
    try {
      const grant = await pocket.fs.requestFolder('sd');
      if (grant !== 'sd:/') throw new Error('folder grant declined');
      try {
        await pocket.fs.stat(path);
        throw new Error('test filename already exists; refusing collision');
      } catch (error) {
        if (error.code !== 'NOT_FOUND') throw error;
      }
      file = await pocket.fs.open(path,
                                  {mode: 'create', durability: 'synced'});
      await file.write(header());
      await pocket.time.sleep(20);
      for (let i = 0; i < frames; ++i) {
        await file.write(framePacket(i));
        // A real timer keeps each generated frame and write out of the same
        // QuickJS promise drain; a plain await can settle in that drain.
        await pocket.time.sleep(20);
        if ((i + 1) % 10 === 0)
          console.log('D5_STREAM WRITE frames=' + (i + 1));
      }
      await file.commit(); file = null; owned = true;
      console.log('D5_STREAM CREATED bytes=' + (16 + frames * (12 + width * height * 2)));
      video.streamStart(path); stream = true;
      readyAt = pocket.time.now();
      console.log('D5_STREAM START');
    } catch (error) {
      console.log('D5_STREAM ERROR prepare ' + ((error && error.code) || error));
      await finish('prepare-error');
    }
  }
  globalThis.frame = function () {
    if (!started) { started = true; prepare(); }
    if (!stream || finishing || !readyAt) return;
    const elapsed = pocket.time.now() - readyAt;
    if (!paused && elapsed >= 700) {
      video.streamPause(true); paused = true;
      console.log('D5_STREAM PAUSE');
    }
    if (paused && !resumed && elapsed >= 900) {
      video.streamPause(false); resumed = true;
      console.log('D5_STREAM RESUME');
    }
    const state = video.streamState();
    if (state === 'error') { finish('stream-error'); return; }
    if (video.streamPoll(Math.floor(elapsed * 1000))) {
      ++selected;
      console.log('D5_STREAM SELECT n=' + selected);
    }
    if (state === 'end' && elapsed >= 2200) {
      console.log('D5_STREAM EOF');
      finish('end');
    } else if (elapsed >= 10000) finish('timeout');
  };
})();
