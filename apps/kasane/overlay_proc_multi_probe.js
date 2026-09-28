// Diagnostic-only deskclock overlay: two procedural images over live FLOWER.
(() => {
  const view = pocket.kasane;
  const proc = view.procedural;
  const second = proc.createSurface();
  const left = proc.resource();
  const right = proc.resource(second);
  const red = proc.register([
    [1, 0, 0, 0, 0, 0], [1, 1, 1, 0, 0, 0],
    [8, 0, 0, 1, 0, 0xf800],
  ]);
  const green = proc.register([
    [1, 0, 0, 0, 0, 0], [1, 1, 1, 0, 0, 0],
    [8, 0, 0, 1, 0, 0x07e0],
  ]);
  view.replace(tx => {
    tx.background(0x07101cff);
    tx.image({resource: left, bounds: [2, 4, 34, 16], scale: 1,
              sourceWidth: 32, sourceHeight: 12});
    tx.image({resource: right, bounds: [54, 4, 86, 16], scale: 1,
              sourceWidth: 32, sourceHeight: 12});
  });
  console.log('D2_OVERLAY START surfaces=2 image_nodes=2');
  let tick = 0;
  globalThis.frame = function () {
    const slot = tick & 1;
    const x = 6 + ((tick >> 1) % 16);
    if (slot) proc.beginFrame(0, second);
    else proc.beginFrame(0);
    proc.draw(slot ? green : red, [x, 7, 0, 0]);
    proc.commit();
    if (tick < 8) console.log('D2_OVERLAY COMMIT tick=' + tick + ' slot=' + slot);
    ++tick;
    if (tick === 8) console.log('D2_OVERLAY FRAME 8');
  };
})();
