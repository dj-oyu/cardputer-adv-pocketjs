// Ordinary APP experiment for registered RGB565 resize and symbolic grid fold.
// Enter advances the mode; it also cycles automatically.
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
  const foldWidth = 48, foldHeight = 28;
  const foldIndex = gridFold.index(0, 1, foldWidth, 0, 0);
  const mirrorIndex = gridFold.index(foldWidth - 1, -1, foldWidth, 0, 0);
  const foldProgram = gridFold.fold({
    width: foldWidth, height: foldHeight, tapWidth: 1, tapHeight: 1,
    output: foldIndex, shift: 2
  }, g => {
    const sample = g.load(0, foldIndex);
    const mirror = g.load(0, mirrorIndex);
    return g.add(g.acc,
                 g.add(g.mul(sample, g.constant(2)), mirror));
  });
  modes.push({width: foldWidth, height: foldHeight, label: 'FOLD ART',
              kind: 'fold', sourceWidth: foldWidth,
              source: new Int16Array(foldWidth * foldHeight),
              handle: grid.register(foldProgram), resource: null});
  const pairWidth = 48, pairHeight = 20, pairPitch = 64;
  const pairProgram = gridFold.fold({
    width: pairWidth, height: pairHeight, tapWidth: 1, tapHeight: 1,
    output: gridFold.index(0, 1, pairWidth, 0, 0), shift: 2
  }, g => g.add(g.acc, g.add(
    g.mul(g.load(0, gridFold.index(0, 1, pairPitch, 0, 0)),
          g.constant(2)),
    g.load(0, gridFold.index(8, 1, pairPitch, 0, 0)))));
  modes.push({width: pairWidth, height: pairHeight, label: 'FOLD PAIR',
              kind: 'fold', sourceWidth: pairPitch,
              source: new Int16Array(pairPitch * pairHeight),
              handle: grid.register(pairProgram), resource: null});
  const weightWidth = 40, weightHeight = 20;
  const weightProgram = gridFold.fold({
    width: weightWidth, height: weightHeight, tapWidth: 1, tapHeight: 1,
    output: gridFold.index(0, 1, weightWidth, 0, 0), shift: 2
  }, g => g.add(g.acc, g.add(
    g.mul(g.load(0, gridFold.index(0, 1, weightWidth, 0, 0)),
          g.load(2, gridFold.index(0, 1, weightWidth, 0, 0))),
    g.load(0, gridFold.index(weightWidth - 1, -1,
                             weightWidth, 0, 0)))));
  modes.push({width: weightWidth, height: weightHeight, label: 'FOLD WEIGHT',
              kind: 'fold', sourceWidth: weightWidth,
              source: new Int16Array(weightWidth * weightHeight),
              weights: new Int16Array(weightWidth * weightHeight),
              handle: grid.register(weightProgram), resource: null});
  const foldColors = new Int16Array(32);
  for (let v = 0; v < 32; ++v)
    foldColors[v] = ((v >> 1) << 11) | ((v * 2) << 5) | (31 - v);
  for (const mode of modes) {
    if (mode.kind !== 'fold') continue;
    mode.phase = new Uint8Array(mode.source.length);
    for (let y = 0; y < mode.height; ++y)
      for (let x = 0; x < mode.sourceWidth; ++x)
        mode.phase[y * mode.sourceWidth + x] =
          (x * 3 + y * 7 + ((x ^ y) & 15)) & 31;
  }
  let tick = 0, selected = -1, enterHeld = false;
  globalThis.frame = function (buttons) {
    const enter = !!(buttons & 0x4000);
    const advanced = enter && !enterHeld;
    if (advanced) selected = (selected + 1) % modes.length;
    else if (selected < 0 || tick % 120 === 0)
      selected = (selected + 1) % modes.length;
    enterHeld = enter;
    const mode = modes[selected];
    if (mode.kind === 'fold') {
      for (let i = 0; i < mode.source.length; ++i)
        mode.source[i] = foldColors[(mode.phase[i] + tick) & 31];
      if (mode.weights)
        for (let i = 0; i < mode.weights.length; ++i)
          mode.weights[i] = 1 + ((i + tick) & 3);
    } else {
      for (let y = 0; y < sourceHeight; ++y) {
        for (let x = 0; x < sourceWidth; ++x) {
          const v = (x * 5 + y * 9 + tick * 2 + ((x ^ y) & 15)) & 63;
          source[y * sourceWidth + x] =
            (((v >> 1) & 31) << 11) | (v << 5) | ((63 - v) >> 1);
        }
      }
    }
    const buffers = {0: mode.source || source};
    if (mode.weights) buffers[2] = mode.weights;
    const backend = grid.run(mode.handle, buffers);
    if (!mode.resource) mode.resource = grid.resource(mode.handle);
    const inspect = tick === 0 || tick % 120 === 0 || advanced;
    if (mode.kind === 'fold' && inspect) {
      mode.measure = grid.measure(mode.handle, 8);
      mode.gather = grid.measure(mode.handle, 8, 'GATHER');
      mode.affine = grid.measure(mode.handle, 8, 'AFFINE');
      console.log('GRID_APP MEASURE ' + selected +
                  ' repeats=' + mode.measure.repeats +
                  ' scalar_us=' + mode.measure.scalarUs +
                  ' pie_us=' + mode.measure.pieUs +
                  ' equal=' + (mode.measure.equal ? 1 : 0));
      console.log('GRID_APP ROUTES ' + selected +
                  ' gather_us=' + mode.gather.pieUs +
                  ' affine_us=' + mode.affine.pieUs +
                  ' equal=' + (mode.gather.equal && mode.affine.equal ? 1 : 0));
    }
    if (inspect) {
      const left = Math.floor((240 - mode.width * 2) / 2);
      view.replace(tx => {
        tx.background(0x061322ff);
        tx.text({bounds: [8, 2, 232, 16],
                 text: 'GRID LAB  /  ' + mode.label,
                 font: 'caption', color: 0xd7f4ffff});
        tx.image({resource: mode.resource,
                  bounds: [left, 20, left + mode.width * 2,
                           20 + mode.height * 2],
                  scale: 2, sourceWidth: mode.width,
                  sourceHeight: mode.height});
        if (mode.measure) tx.text({bounds: [8, 82, 232, 98],
                   text: 'S:' + Math.round(mode.measure.scalarUs / 8) +
                         ' G:' + Math.round(mode.gather.pieUs / 8) +
                         ' A:' + Math.round(mode.affine.pieUs / 8) + ' us',
                   font: 'caption', color: 0xffd47aff});
        tx.text({bounds: [8, 99, 232, 115],
                 text: 'ENTER: NEXT  /  AUTO: 120 FRAMES',
                 font: 'caption', color: 0xa6bdc8ff});
        tx.text({bounds: [8, 117, 232, 133],
                 text: mode.kind === 'fold' ?
                   'FOLD: X*2+MIRROR(X) / ' + backend :
                   'RGB565  /  BILINEAR  /  ' + backend,
                 font: 'caption', color: 0x80d9e8ff});
      });
      const route = grid.explain(mode.handle);
      console.log('GRID_APP MODE ' + selected + ' ' + mode.width + 'x' +
                  mode.height + ' backend=' + backend +
                  ' strategy=' + route.strategy +
                  ' reason=' + route.reason +
                  ' key=' + route.profileKey);
    } else if (tick % 48 === 0) {
      console.log('GRID_APP FRAME ' + tick + ' backend=' + backend);
    }
    tick = (tick + 1) % 48000;
  };
})();
