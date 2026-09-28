// Diagnostic only: JS frames over the native FLOWER backdrop through the
// ordinary Kasane image resource. The 96x22 deskclock viewport stays small.
(() => {
  const view = pocket.kasane;
  const video = view.video;
  const width = 32, height = 8;
  const resource = video.open(width, height);
  const pixels = new Uint16Array(width * height);
  let tick = 0;
  let audioRequested = false;
  let player = null;
  function audioError(stage, error) {
    console.log('D6_SD_MP3_ERROR stage=' + stage + ' code=' +
                ((error && error.code) || error));
  }
  pocket.overlay.onKey(function (event) {
    if (event.action !== 'accept' || audioRequested) return;
    audioRequested = true;
    console.log('D6_SD_MP3_REQUEST');
    pocket.fs.requestFolder('sd').then(function (root) {
      if (root !== 'sd:/') throw new Error('grant declined');
      console.log('D6_SD_MP3_GRANTED');
      return pocket.fs.pickFile('sd', {extensions: ['.mp3']});
    }).then(function (path) {
      if (!path) throw new Error('file declined');
      console.log('D6_SD_MP3_PICKED ' + path);
      return pocket.audio.player.open({source: path});
    }).then(function (handle) {
      player = handle;
      const info = handle.info();
      console.log('D6_SD_MP3_OPEN codec=' + info.codec +
                  ' durationMs=' + info.durationMs);
      if (info.codec !== 'mp3') throw new Error('not MP3');
      return handle.play();
    }).then(function () {
      console.log('D6_SD_MP3_PLAY');
    }).catch(function (error) { audioError('START', error); });
  });
  view.replace(tx => {
    tx.background(0x000000ff);
    tx.image({resource, bounds: [2, 2, 66, 18], scale: 2});
    tx.rect({bounds: [42, 4, 63, 16], color: 0xffe08080});
    tx.text({bounds: [69, 4, 94, 17], text: 'D6', font: 'caption',
             color: 0xe7f4ffff});
  });
  globalThis.frame = function () {
    const base = (((tick >> 1) & 31) << 11) | 0x0212;
    pixels.fill(base);
    const left = tick % (width - 4);
    for (let y = 0; y < height; ++y)
      pixels.fill(0x07ff, y * width + left, y * width + left + 4);
    const pts = tick * 100000;
    if (!video.push(pixels, pts)) {
      console.log('D6_VIDEO_BUSY tick=' + tick);
    } else if (video.select(pts) && tick % 10 === 0) {
      console.log('D6_VIDEO_FRAME tick=' + tick);
    }
    ++tick;
  };
})();
