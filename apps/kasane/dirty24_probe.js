// P2 diagnostic only. A foreground mount exercises all 24 slot declarations,
// topology changes, and A -> B -> A pixels without a second JS scene model.
(function () {
  const slots = {};
  const initial = {};
  const nodes = [];
  for (let i = 0; i < 22; i++) {
    const name = 'v' + i;
    slots[name] = {type: 'text', capacity: i === 0 ? 47 : 6};
    initial[name] = ('0' + i).slice(-2);
    const col = i & 1;
    const row = i >> 1;
    const x = col ? 124 : 4;
    const y = 2 + row * 12;
    nodes.push({type: 'text', bounds: [x, y, x + 111, y + 11],
      text: {slot: name}, color: 0xe2f0ffff});
  }
  slots.show = {type: 'bool'};
  slots.page = {type: 'u16', initial: 0, maximum: 1};
  initial.show = false;
  initial.page = 0;
  nodes.push({type: 'rect', bounds: [114, 0, 122, 135],
    color: 0xff8040ff, visible: {slot: 'show'}});
  nodes.push({type: 'rect', bounds: [0, 132, 240, 135],
    color: 0x40ff80ff, page: {slot: 'page'}, pageEquals: 1});
  const view = pocket.kasane.mount({version: 1, background: 0x071425ff,
    slots: slots, nodes: nodes}, initial);
  let stage = 0;
  console.log('KSN_DIRTY24 READY slots=24 nodes=24 stage=0');
  globalThis.frame = function (buttons) {
    if (buttons & 0x10) {
      if (stage === 0) {
        const final = {v1: 'B01'};
        for (let i = 0; i < 22; i += 2)
          final['v' + i] = 'D' + ('0' + i).slice(-2);
        view.set(final);
        console.log('KSN_DIRTY24 COMPOSITE_DIRECT');
      } else {
        view.set({v0: 'SECOND', v2: 'B02'});
        console.log('KSN_DIRTY24 DIRECT');
      }
      return;
    }
    if (buttons & 0x40) {
      const first = {};
      const final = {v1: 'B01'};
      for (let i = 0; i < 22; i += 2) {
        const name = 'v' + i;
        const suffix = ('0' + i).slice(-2);
        first[name] = 'C' + suffix;
        final[name] = 'D' + suffix;
      }
      view.set(first);
      view.set(final);
      console.log('KSN_DIRTY24 COMPOSITE_BURST');
      return;
    }
    if (buttons & 0x80) {
      view.set({v0: 'FIRST'});
      view.set({v0: 'SECOND', v2: 'B02'});
      console.log('KSN_DIRTY24 BURST');
      return;
    }
    if (buttons & 0x20) {
      const changed = {};
      for (let i = 0; i < 22; i += 2)
        changed['v' + i] = 'C' + ('0' + i).slice(-2);
      view.set(changed);
      console.log('KSN_DIRTY24 COLUMN');
      return;
    }
    if (!(buttons & 0x4000)) return;
    stage++;
    if (stage === 1) view.set({v0: 'X'.repeat(47)});
    else if (stage === 2) {
      const changed = {show: true, page: 1};
      for (let i = 0; i < 22; i++) changed['v' + i] = 'S' + ('0' + i).slice(-2);
      view.set(changed);
    } else if (stage === 3) view.set(initial);
    else view.set({v0: 'T' + (stage & 1)});
    if (stage <= 3 || (stage - 3) % 30 === 0)
      console.log('KSN_DIRTY24 STAGE ' + stage);
  };
})();
