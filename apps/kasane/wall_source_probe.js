// USB diagnostic 7. The app supplies only a generic schema and a source-to-slot
// map; no JS frame reconstructs the displayed time.
(function () {
  const view = pocket.kasane.mount({version: 1,
    slots: {face: {type: 'text', capacity: 5},
            tag: {type: 'text', capacity: 7}},
    nodes: [
      {type: 'rect', bounds: [0, 0, 96, 22], color: 0x060c1aff},
      {type: 'text', bounds: [4, 4, 48, 16], text: {slot: 'face'}, color: 0xe2f0ffff},
      {type: 'text', bounds: [50, 12, 94, 24], text: {slot: 'tag'}, color: 0x63d6ddff}
    ]}, {face: 'BASE', tag: 'BASE'});
  const source = pocket.time.wallSource();
  let bound = false;
  globalThis.frame = function () {
    if (bound) return;
    try { view.bind(source, {face: 0, tag: 1}); }
    catch (error) {
      if (error.code === 'BUSY') return; // Initial mount not presented yet.
      console.log('KSN_WALL_SOURCE ERROR ' + error);
      throw error;
    }
    bound = true;
    console.log('KSN_WALL_SOURCE BOUND unixMs=' + pocket.time.wall().unixMs);
  };
})();
