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
  const foldIndex = gridFold.index({x: 1, y: foldWidth});
  const mirrorIndex = gridFold.index({base: foldWidth - 1,
                                     x: -1, y: foldWidth});
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
    output: gridFold.index({x: 1, y: pairWidth}), shift: 2
  }, g => g.add(g.acc, g.add(
    g.mul(g.load(0, gridFold.index({x: 1, y: pairPitch})),
          g.constant(2)),
    g.load(0, gridFold.index({base: 8, x: 1, y: pairPitch})))));
  modes.push({width: pairWidth, height: pairHeight, label: 'FOLD PAIR',
              kind: 'fold', sourceWidth: pairPitch,
              source: new Int16Array(pairPitch * pairHeight),
              handle: grid.register(pairProgram), resource: null});
  const weightWidth = 40, weightHeight = 20;
  const weightProgram = gridFold.fold({
    width: weightWidth, height: weightHeight, tapWidth: 1, tapHeight: 1,
    output: gridFold.index({x: 1, y: weightWidth}), shift: 2
  }, g => g.add(g.acc, g.add(
    g.mul(g.load(0, gridFold.index({x: 1, y: weightWidth})),
          g.load(2, gridFold.index({x: 1, y: weightWidth}))),
    g.load(0, gridFold.index({base: weightWidth - 1,
                             x: -1, y: weightWidth})))));
  modes.push({width: weightWidth, height: weightHeight, label: 'FOLD WEIGHT',
              kind: 'fold', sourceWidth: weightWidth,
              source: new Int16Array(weightWidth * weightHeight),
              weights: new Int16Array(weightWidth * weightHeight),
              handle: grid.register(weightProgram), resource: null});
  const art = modes[3];
  for (const [full, route, label] of [
    [false, 'AUTO', 'PIPE WIN PIE'], [false, 'SCALAR', 'PIPE WIN CPU'],
    [true, 'AUTO', 'PIPE FULL PIE'], [true, 'SCALAR', 'PIPE FULL CPU']
  ]) modes.push({width: art.width, height: art.height, label,
                 kind: 'fold', sourceWidth: art.sourceWidth,
                 source: art.source, handle: art.handle, resource: null,
                 profileSource: art, profile: true, full, route});
  for (let index = 3; index < 6; ++index) {
    const mode = modes[index];
    mode.registration = grid.registration(mode.handle);
    const r = mode.registration;
    console.log('GRID_APP REGISTER ' + index +
                ' ir=' + r.irCount + ' plan=' + r.planBytes +
                ' analysis=' + r.analysisBytes +
                ' parse_us=' + r.parseUs +
                ' prepare_us=' + r.prepareUs +
                ' total_us=' + r.totalUs +
                ' heap_before=' + r.heapBefore +
                ' heap_plan=' + r.heapAfterPlan +
                ' heap_after=' + r.heapAfter +
                ' largest_before=' + r.largestBefore +
                ' largest_after=' + r.largestAfter);
  }
  const foldColors = new Int16Array(32);
  for (let v = 0; v < 32; ++v)
    foldColors[v] = ((v >> 1) << 11) | ((v * 2) << 5) | (31 - v);
  for (const mode of modes) {
    if (mode.kind !== 'fold') continue;
    if (mode.profile) { mode.phase = mode.profileSource.phase; continue; }
    mode.phase = new Uint8Array(mode.source.length);
    for (let y = 0; y < mode.height; ++y)
      for (let x = 0; x < mode.sourceWidth; ++x)
        mode.phase[y * mode.sourceWidth + x] =
          (x * 3 + y * 7 + ((x ^ y) & 15)) & 31;
  }
  let tick = 0, selected = -1, modeTick = 0, enterHeld = false;
  let sourceUs = 0, runUs = 0, samples = 0, lastFrameMs = 0;
  let maxGapUs = 0, gapsOver50 = 0, gapsOver75 = 0;
  const finishProfile = mode => {
    const p = grid.profile(mode.handle);
    console.log('GRID_APP PIPE arm=' + (selected - 6) +
                ' layout=' + (mode.full ? 'FULL' : 'WINDOW') +
                ' route=' + mode.route + ' frames=' + samples +
                ' source_us=' + Math.round(sourceUs) +
                ' run_us=' + Math.round(runUs) +
                ' native_copy_us=' + p.copyUs +
                ' bind_us=' + p.bindUs + ' kernel_us=' + p.kernelUs +
                ' native_total_us=' + p.totalUs +
                ' native_runs=' + p.runs);
    console.log('GRID_APP MEM arm=' + (selected - 6) +
                ' native_max_us=' + p.maxTotalUs +
                ' heap_free=' + p.heapFree +
                ' heap_largest=' + p.heapLargest +
                ' heap_min_free=' + p.heapMinFree +
                ' max_gap_us=' + Math.round(maxGapUs) +
                ' gaps_over_50ms=' + gapsOver50 +
                ' gaps_over_75ms=' + gapsOver75);
  };
  globalThis.frame = function (buttons) {
    const enter = !!(buttons & 0x4000);
    const advanced = enter && !enterHeld;
    if (advanced || selected < 0 ||
        modeTick >= (modes[selected].profile ? 180 : 120)) {
      if (selected >= 6) finishProfile(modes[selected]);
      selected = (selected + 1) % modes.length;
      modeTick = 0;
      if (modes[selected].profile) {
        grid.profile(modes[selected].handle);
        sourceUs = 0; runUs = 0; samples = 0;
        lastFrameMs = 0; maxGapUs = 0; gapsOver50 = 0; gapsOver75 = 0;
        console.log('GRID_APP ARM ' + (selected - 6) +
                    ' layout=' + (modes[selected].full ? 'FULL' : 'WINDOW') +
                    ' route=' + modes[selected].route);
      }
    }
    enterHeld = enter;
    const mode = modes[selected];
    const sourceStart = mode.profile ? pocket.time.now() : 0;
    if (mode.profile) {
      if (lastFrameMs) {
        const gap = (sourceStart - lastFrameMs) * 1000;
        if (gap > maxGapUs) maxGapUs = gap;
        if (gap > 50000) ++gapsOver50;
        if (gap > 75000) ++gapsOver75;
      }
      lastFrameMs = sourceStart;
    }
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
    const sourceEnd = mode.profile ? pocket.time.now() : 0;
    const backend = mode.profile ?
      grid.run(mode.handle, buffers, undefined, {backend: mode.route}) :
      grid.run(mode.handle, buffers);
    if (mode.profile) {
      sourceUs += (sourceEnd - sourceStart) * 1000;
      runUs += (pocket.time.now() - sourceEnd) * 1000;
      ++samples;
    }
    if (mode.profile && mode.profileSource.resource)
      mode.resource = mode.profileSource.resource;
    if (!mode.resource) mode.resource = grid.resource(mode.handle);
    const inspect = mode.profile ? modeTick === 0 :
                    tick === 0 || modeTick === 0 || advanced;
    if (mode.kind === 'fold' && inspect && !mode.profile) {
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
                  bounds: mode.full ? [0, 0, 240, 135] :
                          [left, 20, left + mode.width * 2,
                           20 + mode.height * 2],
                  scale: mode.full ? undefined : 2,
                  sourceWidth: mode.width,
                  sourceHeight: mode.height});
        if (mode.measure) tx.text({bounds: [8, 82, 232, 98],
                   text: 'S:' + Math.round(mode.measure.scalarUs / 8) +
                         ' G:' + Math.round(mode.gather.pieUs / 8) +
                         ' A:' + Math.round(mode.affine.pieUs / 8) + ' us',
                   font: 'caption', color: 0xffd47aff});
        tx.text({bounds: [8, 99, 232, 115],
                 text: mode.registration ?
                   'REG ' + mode.registration.planBytes + 'B / ' +
                   mode.registration.prepareUs + 'us  ENTER:NEXT' :
                   'ENTER: NEXT  /  AUTO: 120 FRAMES',
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
                  ' key=' + route.profileKey +
                  ' kernel=' + route.kernel +
                  ' scalar_reason=' + route.scalarReason +
                  ' candidate_mask=' + (route.candidateMask || 0));
    } else if (tick % 48 === 0) {
      console.log('GRID_APP FRAME ' + tick + ' backend=' + backend);
    }
    tick = (tick + 1) % 48000;
    ++modeTick;
  };
})();
