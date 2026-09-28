// Ordinary APP experiment for the registered, arbitrary-ratio RGB565 grid
// resampler. Enter advances the ratio; it also cycles automatically.
(() => {
  const view = pocket.kasane;
  const grid = view.grid;
  const sourceWidth = 60, sourceHeight = 30;
  const modes = [
    {width: 28, height: 14, label: '15:7'},
    {width: 37, height: 23, label: '60:37 / 30:23'},
    {width: 30, height: 15, label: '2:1'}
  ];
  const source = new Int16Array(sourceWidth * sourceHeight);
  for (const mode of modes) {
    mode.handle = grid.registerResize({sourceWidth, sourceHeight,
                                       width: mode.width, height: mode.height});
    mode.resource = null;
  }
  let tick = 0, selected = -1, enterHeld = false;
  globalThis.frame = function (buttons) {
    const enter = !!(buttons & 0x4000);
    const advanced = enter && !enterHeld;
    if (advanced) selected = (selected + 1) % modes.length;
    else if (selected < 0 || tick % 120 === 0)
      selected = (selected + 1) % modes.length;
    enterHeld = enter;
    for (let y = 0; y < sourceHeight; ++y) {
      for (let x = 0; x < sourceWidth; ++x) {
        const v = (x * 5 + y * 9 + tick * 2 + ((x ^ y) & 15)) & 63;
        source[y * sourceWidth + x] =
          (((v >> 1) & 31) << 11) | (v << 5) | ((63 - v) >> 1);
      }
    }
    const mode = modes[selected];
    const backend = grid.run(mode.handle, {0: source});
    if (!mode.resource) mode.resource = grid.resource(mode.handle);
    if (tick === 0 || tick % 120 === 0 || advanced) {
      const left = Math.floor((240 - mode.width * 2) / 2);
      view.replace(tx => {
        tx.background(0x061322ff);
        tx.text({bounds: [8, 2, 232, 16],
                 text: 'GRID RESIZE  /  ' + mode.label,
                 font: 'caption', color: 0xd7f4ffff});
        tx.image({resource: mode.resource,
                  bounds: [left, 20, left + mode.width * 2,
                           20 + mode.height * 2],
                  scale: 2, sourceWidth: mode.width,
                  sourceHeight: mode.height});
        tx.text({bounds: [8, 117, 232, 133],
                 text: 'RGB565  /  BILINEAR  /  ' + backend,
                 font: 'caption', color: 0x80d9e8ff});
      });
      const route = grid.explain(mode.handle);
      console.log('GRID_APP MODE ' + selected + ' ' + mode.width + 'x' +
                  mode.height + ' backend=' + backend +
                  ' strategy=' + route.strategy +
                  ' reason=' + route.reason);
    } else if (tick % 48 === 0) {
      console.log('GRID_APP FRAME ' + tick + ' backend=' + backend);
    }
    tick = (tick + 1) % 48000;
  };
})();
