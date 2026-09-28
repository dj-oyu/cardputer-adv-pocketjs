// The same symbolic frontend and 2x2 box expression as grid_fold_examples.js.
// The device probe reads this generated IR through QuickJS, then prepares it
// with the native grid compiler and measures AUTO against forced gather.
globalThis.gridFoldDeviceProgram = gridFold.fold(
  {width: 16, height: 12, tapWidth: 2, tapHeight: 2, shift: 2,
   output: gridFold.index({x: 1, y: {param: 1}})},
  g => g.add(g.acc, g.mul(
    g.load(gridFold.view({buffer: 0, x: 2, y: {param: 0, scale: 2},
                              tapX: 1, tapY: {param: 0}})),
    g.constant(1))));
