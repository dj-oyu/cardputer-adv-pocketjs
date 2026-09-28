// Small RGB565 frame stream inside ordinary Kasane UI. No decoder is involved.
(() => {
  const view = pocket.kasane;
  const video = view.video;
  const width = 64, height = 48;
  const resource = video.open(width, height);
  const pixels = new Uint16Array(width * height);
  const names = ['SWEEP', 'COLOR BARS', 'CHECKER', 'INTERFERENCE'];
  const colors = [0xffff, 0xffe0, 0x07ff, 0x07e0,
                  0xf81f, 0xf800, 0x001f, 0x0000];
  let tick = 0;
  let pattern = 0;
  let title;
  view.replace(tx => {
    tx.background(0x061322ff);
    title = tx.text({bounds: [8, 2, 232, 18], text: '1/4 SWEEP', capacity: 24,
                     font: 'caption', color: 0xe7f4ffff});
    tx.image({resource, bounds: [56, 21, 184, 117], scale: 2});
    tx.text({bounds: [8, 119, 232, 134], text: 'ARROWS: SELECT  /  RGB565',
             font: 'caption', color: 0x80d9e8ff});
  });
  pocket.input.onAction(event => {
    if (event.phase === 'release') return;
    const step = event.action === 'left' || event.action === 'up' ? -1 :
                 event.action === 'right' || event.action === 'down' ? 1 : 0;
    if (!step) return;
    pattern = (pattern + step + names.length) % names.length;
    view.patch(tx => title.setText(tx, (pattern + 1) + '/4 ' + names[pattern]));
    console.log('VIDEO_LAB PATTERN ' + pattern + ' ' + names[pattern]);
  });
  globalThis.frame = function () {
    if (pattern === 0) {
      const background = (((tick >> 2) & 31) << 11) | 0x0212;
      const bar = 0x07ff ^ ((tick & 31) << 11);
      const left = (tick * 2) % (width - 8);
      pixels.fill(background);
      for (let y = 0; y < height; ++y) {
        const row = y * width;
        pixels.fill(bar, row + left, row + left + 8);
      }
    } else if (pattern === 1) {
      for (let y = 0; y < height; ++y) {
        const row = y * width;
        for (let band = 0; band < colors.length; ++band)
          pixels.fill(colors[band], row + band * 8, row + band * 8 + 8);
      }
      const scan = tick % height;
      pixels.fill(0xffff, scan * width, (scan + 1) * width);
    } else if (pattern === 2) {
      const shift = tick >> 1;
      for (let y = 0; y < height; ++y) {
        const row = y * width;
        for (let x = 0; x < width; x += 8) {
          const light = (((x + shift) >> 3) ^ ((y + shift) >> 3)) & 1;
          pixels.fill(light ? 0xffe0 : 0x081f, row + x, row + x + 8);
        }
      }
    } else {
      for (let y = 0; y < height; ++y) {
        const row = y * width;
        for (let x = 0; x < width; x += 4) {
          const a = (x * 3 + tick * 2) & 31;
          const b = (y * 5 - tick) & 63;
          const c = ((x ^ y) + tick) & 31;
          pixels.fill((a << 11) | (b << 5) | c, row + x, row + x + 4);
        }
      }
    }
    if (!video.push(pixels, tick * 33333)) {
      console.log('VIDEO_LAB BUSY ' + tick);
    } else if (video.select(tick * 33333) && tick % 60 === 0) {
      console.log('VIDEO_LAB FRAME ' + tick);
    }
    ++tick;
  };
})();
