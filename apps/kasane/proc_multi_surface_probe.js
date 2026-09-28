// Diagnostic-only ordinary APP: two independent procedural image resources.
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
    tx.background(0x061322ff);
    tx.text({bounds: [5, 3, 235, 19], text: 'D2 / TWO INDEPENDENT SURFACES',
             font: 'caption', color: 0xe7f4ffff});
    tx.image({resource: left, bounds: [16, 31, 80, 79], scale: 1,
              sourceWidth: 64, sourceHeight: 48});
    tx.image({resource: right, bounds: [160, 31, 224, 79], scale: 1,
              sourceWidth: 64, sourceHeight: 48});
    tx.text({bounds: [16, 90, 224, 106], text: 'RED LEFT / GREEN RIGHT',
             font: 'caption', color: 0x80d9e8ff});
  });
  console.log('D2_MULTI START surfaces=2 image_nodes=2');
  let tick = 0;
  globalThis.frame = function () {
    const slot = tick & 1;
    const x = 10 + ((tick >> 1) % 32);
    if (slot) proc.beginFrame(0, second);
    else proc.beginFrame(0);
    proc.draw(slot ? green : red, [x, 20, 0, 0]);
    proc.commit();
    if (tick < 4 || tick % 20 === 0)
      console.log('D2_MULTI COMMIT tick=' + tick + ' slot=' + slot + ' x=' + x);
    ++tick;
    if (tick % 60 === 0) console.log('D2_MULTI FRAME ' + tick);
  };
})();
