// Run explicitly after an interrupted D6 probe. Remove only exact probe bytes.
(async () => {
  const path = 'sd:/__ksn_d6_av_stream_probe_20260928.ksv';
  const width = 32, frames = 300, frameBytes = 512, intervalUs = 33333;
  const size = 16 + frames * (12 + frameBytes);
  const header = [75, 83, 86, 49, 32, 0, 8, 0,
                  0, 2, 0, 0, 0, 0, 0, 0];
  function expected(at) {
    if (at < 16) return header[at];
    const relative = at - 16;
    const frame = Math.floor(relative / (12 + frameBytes));
    const inside = relative % (12 + frameBytes);
    if (inside < 4) return (frame * intervalUs >>> (inside * 8)) & 255;
    if (inside < 8) return 0;
    if (inside < 12) return (frameBytes >>> ((inside - 8) * 8)) & 255;
    const pixel = Math.floor((inside - 12) / 2);
    const x = pixel % width;
    const color = x >= (frame % width) && x < (frame % width) + 4
      ? 0xffe0 : (((frame & 31) << 11) | 0x03ff);
    return (color >>> (((inside - 12) % 2) * 8)) & 255;
  }
  let file = null;
  try {
    if (await pocket.fs.requestFolder('sd') !== 'sd:/')
      throw new Error('folder grant declined');
    const entry = await pocket.fs.stat(path);
    if (entry.sizeBytes !== size)
      throw new Error('size mismatch; refusing removal');
    file = await pocket.fs.open(path, {mode: 'read'});
    for (let at = 0; at < size;) {
      const chunk = await file.read(Math.min(1024, size - at));
      if (!chunk || !chunk.length)
        throw new Error('early EOF; refusing removal');
      for (let j = 0; j < chunk.length; ++j)
        if (chunk[j] !== expected(at + j))
          throw new Error('content mismatch; refusing removal');
      at += chunk.length;
      await pocket.time.sleep(20);
    }
    if (await file.read(1) !== null)
      throw new Error('trailing bytes; refusing removal');
    file.close(); file = null;
    await pocket.fs.remove(path);
    console.log('D6_AV RECOVER_REMOVED');
  } catch (error) {
    console.log('D6_AV RECOVER_REFUSED ' + ((error && error.code) || error));
  } finally {
    if (file) file.close();
  }
})();
