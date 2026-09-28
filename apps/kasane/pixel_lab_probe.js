// Diagnostic-only ordinary APP. Two nodes share one logical pixel frame.
(() => {
  const view = pocket.kasane;
  const pixel = view.pixel;
  const resource = pixel.open(112, 63);
  const params = new Uint16Array(8);
  const code = new Uint16Array([
    2, 0, 0, 0, 0,      // X -> r0
    3, 1, 0, 0, 0,      // Y -> r1
    4, 2, 0, 0, 0,      // params[0] -> r2
    6, 3, 0, 2, 0,      // r0 + r2 -> r3
    8, 4, 1, 2, 0,      // r1 * r2 -> r4
    12, 5, 3, 4, 0,     // r3 xor r4 -> r5
    1, 6, 0, 0, 255,    // opaque alpha -> r6
    12, 7, 5, 2, 0,     // r5 xor r2 -> r7 (RGB565)
  ]);
  let generation = 1;
  params[0] = generation;
  if (!pixel.stage(code, params, 7, 6)) throw Error('D4 initial stage busy');
  view.replace(tx => {
    tx.background(0x061322ff);
    tx.text({bounds: [5, 4, 235, 20], text: 'D4 PIXEL / 2 NODES',
             font: 'caption', color: 0xe7f4ffff});
    tx.image({resource, bounds: [4, 35, 116, 98], scale: 1});
    tx.image({resource, bounds: [124, 35, 236, 98], scale: 1});
  });
  console.log('D4_PIXEL_APP START 112x63x8 nodes=2');
  let tick = 0;
  globalThis.frame = function () {
    ++tick;
    if (tick % 10 === 0) {
      params[0] = ++generation;
      if (pixel.stage(code, params, 7, 6))
        console.log('D4_PIXEL_APP STAGE ' + generation);
      else
        console.log('D4_PIXEL_APP BUSY ' + generation);
    }
    if (tick % 60 === 0) console.log('D4_PIXEL_APP FRAME ' + tick);
  };
})();
