// Real PocketJS host operations, evaluated by quickjs-ng on the Cardputer.
// A compact first app deliberately avoids a framework bundle in the SRAM budget.
(function () {
  const P = { width: 1, height: 2, position: 24, top: 25, left: 28,
    background: 64, radius: 68, color: 96, font: 97 };
  function node(kind, x, y, w, h, color, text) {
    const id = ui.createNode(kind);
    ui.setProp(id, P.position, 1);
    ui.setProp(id, P.left, x); ui.setProp(id, P.top, y);
    ui.setProp(id, P.width, w); ui.setProp(id, P.height, h);
    ui.setProp(id, kind === 1 ? P.color : P.background, color);
    if (text !== undefined) ui.setText(id, text);
    ui.insertBefore(1, id, 0);
    return id;
  }
  ui.setProp(1, P.background, 0x071425ff);
  node(1, 16, 12, 210, 10, 0x69cdeeff, 'POCKETJS / JAVASCRIPT');
  const title = node(1, 16, 34, 220, 18, 0xf0f8ffff, 'Hello, World!');
  ui.setProp(title, P.font, 1);
  const tile = node(0, 16, 62, 208, 38, 0x12334aff);
  ui.setProp(tile, P.radius, 6);
  const counter = node(1, 28, 77, 184, 12, 0x8ef0c4ff, 'KEY PRESSES: 0');
  node(1, 16, 117, 220, 10, 0xa9bacaff, 'ENTER +1    ESC HOME');
  let count = 0;
  let previous = 0;
  globalThis.frame = function (buttons) {
    if ((buttons & 0x4000) && !(previous & 0x4000)) {
      count++;
      ui.setText(counter, 'KEY PRESSES: ' + count);
      console.log('HELLO_COUNT ' + count);
    }
    previous = buttons;
  };
  console.log('HELLO_READY');
})();
