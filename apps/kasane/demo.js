// USB diagnostic K: the first end-to-end user of pocket.kasane. It keeps the
// scene changing so timing captures include alpha, grouped opacity, cache
// placement, replace/patch reference turnover, and modal transitions.
(function () {
  const view = pocket.kasane;
  const tile = view.cache.create([
    {bounds: [0, 0, 42, 24], color: 0x185071ff},
    {bounds: [4, 4, 38, 20], color: 0x63d7bccc, opacity: 220}
  ]);
  let orb, meter, groupFirst, left, right, panel;
  let phase = 'app';
  let tick = 0;

  function base(tx) {
    tx.background(0x071425ff);
    tx.rect({bounds: [0, 0, 240, 18], color: 0x0d2940ff});
    groupFirst = tx.rect({bounds: [12, 28, 112, 56], color: 0x164c70d8});
    tx.rect({bounds: [34, 36, 134, 64], color: 0x65d7bcac});
    tx.group(groupFirst, 2, 208);
    orb = tx.rect({bounds: [16, 76, 36, 96], color: 0xf5bd4fff, opacity: 210});
    meter = tx.rect({bounds: [12, 119, 24, 127], color: 0x62e0a8ff});
    left = tx.instantiate(tile, {offset: [142, 28], opacity: 230});
    right = tx.instantiate(tile, {offset: [188, 72], opacity: 175});
  }

  view.replace(base);
  globalThis.frame = function (buttons) {
    tick++;
    const x = 12 + (tick * 3 % 184);
    const width = 12 + (tick * 5 % 208);
    if (tick === 90) {
      view.replace(function (tx) {
        base(tx);
        tx.modal.open({backdrop: 'dim-live', color: 0x06101a9c, focus: 1});
        panel = tx.rect({bounds: [40, 35, 200, 105], color: 0x397391dc, opacity: 232});
        tx.rect({bounds: [48, 43, 192, 97], color: 0xb8efff42, opacity: 150});
      });
      phase = 'modal';
    } else if (tick === 210) {
      view.replace(function (tx) { base(tx); tx.modal.close(); });
      phase = 'app';
    } else {
      view.patch(function (tx) {
        orb.setRect(tx, [x, 76, x + 20, 96]);
        meter.setRect(tx, [12, 119, 12 + width, 127]);
        groupFirst.setColor(tx, (tick & 16) ? 0x295d86d8 : 0x164c70d8);
        left.place(tx, {offset: [142, 28 + (tick % 18)], opacity: 230});
        right.setVisible(tx, (tick % 40) < 31);
        if (phase === 'modal')
          panel.setColor(tx, (tick & 8) ? 0x397391dc : 0x316781dc);
      });
    }
    if ((tick % 60) === 0) {
      const s = view.stats();
      console.log('KASANE_TICK ' + tick + ' scope=' + view.inputScope() +
                  ' commands=' + s.displayed.commands + ' native=' + s.nativeBytes);
    }
    if (buttons & 0x4000) console.log('KASANE_ENTER ' + tick);
  };
  console.log('KASANE_READY active=' + view.stats().active);
})();
