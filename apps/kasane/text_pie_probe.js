// Diagnostic-only workload: both 8-aligned Japanese text rows change from
// their first glyph on each accept, keeping binary-mask PIE busy every frame.
(function () {
  const a = ['あいうえおかきくけこさしすせそ', 'たちつてとなにぬねのはひふへほ'];
  const b = ['まみむめもやゆよらりるれろわを', 'がぎぐげござじずぜぞだぢづでど'];
  const view = pocket.kasane.mount({
    version: 1,
    background: 0x10283dff,
    slots: {top: {type: 'text', capacity: 47}, bottom: {type: 'text', capacity: 47}},
    nodes: [
      {type: 'text', bounds: [8, 16, 232, 34], font: 1,
        text: {slot: 'top'}, color: 0xe2f0ffb7},
      {type: 'text', bounds: [8, 48, 232, 66], font: 1,
        text: {slot: 'bottom'}, color: 0xf4b86fcd}
    ]
  }, {top: a[0], bottom: b[0]});
  let step = 0;
  console.log('TEXT_PIE_READY');
  globalThis.frame = function (buttons) {
    if (!(buttons & 0x4000)) return;
    step++;
    view.set({top: a[step & 1], bottom: b[step & 1]});
    console.log('TEXT_PIE_STEP ' + step);
  };
})();
