// The same symbolic frontend and 2x2 box expression as grid_fold_examples.js.
// The device probe reads this generated IR through QuickJS, then prepares it
// with the native grid compiler and measures AUTO against forced gather.
globalThis.gridFoldDeviceProgram = gridFold.fold(
  {width: 16, height: 12, tapWidth: 2, tapHeight: 2, shift: 2,
   output: gridFold.index(0, 1, [0, 1, 1], 0, 0)},
  g => g.add(g.acc, g.mul(
    g.load(gridFold.view(0, {x: 2, y: [0, 2, 0], tapX: 1,
                              tapY: [0, 1, 0]})),
    g.constant(1))));
