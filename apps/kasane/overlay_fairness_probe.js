// P5 diagnostic only: a 10 ms native producer drives an overlay mount while
// frame() reports guest service gaps. No JS writes the source-owned UI fields.
(function () {
  const view = pocket.kasane.mount({version: 1,
    slots: {face: {type: 'text', capacity: 5},
            tag: {type: 'text', capacity: 4}},
    nodes: [
      {type: 'rect', bounds: [0, 0, 96, 22], color: 0x060c1aff},
      {type: 'text', bounds: [4, 4, 48, 16], text: {slot: 'face'}, color: 0xe2f0ffff},
      {type: 'text', bounds: [50, 4, 94, 16], text: {slot: 'tag'}, color: 0x63d6ddff}
    ]}, {face: 'BASE', tag: 'BASE'});
  const source = pocket.time.poolProbeSource();
  let bound = false;
  let previous = 0;
  let lastReport = 0;
  let frames = 0;
  let maxGap = 0;
  globalThis.frame = function () {
    if (!bound) {
      try { view.bind(source, {face: 0, tag: 1}); }
      catch (error) {
        if (error.code === 'BUSY') return;
        console.log('KSN_FAIR ERROR bind ' + error);
        throw error;
      }
      bound = true;
      console.log('KSN_FAIR BOUND');
    }
    const now = pocket.time.now();
    if (previous) maxGap = Math.max(maxGap, now - previous);
    previous = now;
    frames++;
    if (!lastReport || now - lastReport >= 1000) {
      lastReport = now;
      console.log('KSN_FAIR HEARTBEAT frames=' + frames + ' maxGapMs=' + maxGap);
    }
  };
})();
