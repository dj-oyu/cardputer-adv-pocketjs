// P4 diagnostic only: keep 23 converted UTF-8 values live through one set().
// The 23rd text node is hidden, so 22 * 39 B fits the APP 896 B text budget.
(function () {
  const slots = {show: {type: 'bool'}};
  const initial = {show: false};
  const nodes = [];
  for (let i = 0; i < 23; i++) {
    const name = 't' + i;
    slots[name] = {type: 'text', capacity: 39};
    initial[name] = 'I' + i;
    const x = (i & 1) ? 124 : 4;
    const y = 2 + (i >> 1) * 11;
    const node = {type: 'text', bounds: [x, y, x + 111, y + 10],
      text: {slot: name}, color: 0xe2f0ffff};
    if (i === 22) node.visible = {slot: 'show'};
    nodes.push(node);
  }
  const view = pocket.kasane.mount({version: 1, background: 0x071425ff,
    slots: slots, nodes: nodes}, initial);
  let stage = 0;
  let update = 0;
  console.log('KSN_TEXT23 READY slots=24 text=23 visible=22');
  globalThis.frame = function (buttons) {
    if (!(buttons & 0x4000)) return;
    stage++;
    if (stage === 3) {
      let rejected = false;
      try { view.set({show: true}); } catch (_) { rejected = true; }
      if (!rejected) throw Error('897-byte visible text was accepted');
      console.log('KSN_TEXT23 PREFLIGHT_REJECT stage=3');
      return;
    }
    update++;
    const changed = {};
    for (let i = 0; i < 23; i++)
      changed['t' + i] = ((update & 1) ? 'A' : 'B') + 'あ'.repeat(12) + 'BC';
    view.set(changed);
    if (stage <= 4 || stage % 30 === 0)
      console.log('KSN_TEXT23 STAGE ' + stage);
  };
})();
