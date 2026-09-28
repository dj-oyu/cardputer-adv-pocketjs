// Diagnostic full-app stage. Never replace an existing reserved filename.
(() => {
  const path = 'sd:/__ksn_d6_av_stream_probe_20260928.ksv';
  const width = 32, height = 8, frames = 300, frameBytes = 512;
  let started = false, file = null;
  function u16(out, at, value) {
    out[at] = value & 255; out[at + 1] = (value >>> 8) & 255;
  }
  function u32(out, at, value) {
    u16(out, at, value); u16(out, at + 2, value >>> 16);
  }
  function header() {
    const out = new Uint8Array(16);
    out.set([75, 83, 86, 49], 0);
    u16(out, 4, width); u16(out, 6, height); u32(out, 8, frameBytes);
    return out;
  }
  function packet(i) {
    const out = new Uint8Array(12 + frameBytes);
    u32(out, 0, i * 33333); u32(out, 4, 0); u32(out, 8, frameBytes);
    for (let j = 0; j < width * height; ++j) {
      const x = j % width;
      const color = x >= (i % width) && x < (i % width) + 4
        ? 0xffe0 : (((i & 31) << 11) | 0x03ff);
      u16(out, 12 + 2 * j, color);
    }
    return out;
  }
  pocket.kasane.replace(tx => {
    tx.background(0x061322ff);
    tx.text({bounds: [8, 2, 232, 18], text: 'D6 SD AV STAGE',
             font: 'caption', color: 0xe7f4ffff});
  });
  async function run() {
    try {
      console.log('D6_STAGE GRANT_REQUEST');
      if (await pocket.fs.requestFolder('sd') !== 'sd:/')
        throw new Error('folder grant declined');
      console.log('D6_STAGE GRANTED');
      try {
        await pocket.fs.stat(path);
        throw new Error('reserved filename collision');
      } catch (error) {
        if (error.code !== 'NOT_FOUND') throw error;
      }
      console.log('D6_STAGE CREATE_START');
      file = await pocket.fs.open(path, {mode: 'create', durability: 'synced'});
      console.log('D6_STAGE OPENED');
      await file.write(header());
      await pocket.time.sleep(20);
      for (let i = 0; i < frames; ++i) {
        await file.write(packet(i));
        await pocket.time.sleep(20);
        if ((i + 1) % 50 === 0)
          console.log('D6_STAGE WRITE frames=' + (i + 1));
      }
      await file.commit(); file = null;
      console.log('D6_STAGE COMMITTED bytes=' +
                  (16 + frames * (12 + frameBytes)) + ' writer=closed');
    } catch (error) {
      if (file) { file.close(); file = null; }
      console.log('D6_STAGE ERROR ' + ((error && error.code) || error));
    }
  }
  globalThis.frame = function () {
    if (started) return;
    started = true;
    run();
  };
})();
