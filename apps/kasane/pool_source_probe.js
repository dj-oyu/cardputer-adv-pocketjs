// USB diagnostic 0. A real FreeRTOS producer writes complete immutable
// snapshots into a fixed pool; the mounted view only declares typed bindings.
(function () {
  const view = pocket.kasane.mount({version: 1,
    slots: {face: {type: 'text', capacity: 5},
            tag: {type: 'text', capacity: 4}},
    nodes: [
      {type: 'rect', bounds: [0, 0, 96, 24], color: 0x060c1aff},
      {type: 'text', bounds: [4, 4, 48, 16], text: {slot: 'face'}, color: 0xe2f0ffff},
      {type: 'text', bounds: [50, 4, 94, 16], text: {slot: 'tag'}, color: 0x63d6ddff}
    ]}, {face: 'BASE', tag: 'BASE'});
  const source = pocket.time.poolProbeSource();
  let bound = false;
  globalThis.frame = function () {
    if (bound) return;
    try { view.bind(source, {face: 0, tag: 1}); }
    catch (error) {
      if (error.code === 'BUSY') return;
      console.log('KSN_POOL_SOURCE ERROR ' + error);
      throw error;
    }
    bound = true;
    console.log('KSN_POOL_SOURCE BOUND');
  };
})();
