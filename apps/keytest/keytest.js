// pocket.input.keys diagnostic. See README.md.
(function () {
  const V = pocket.kasane, K = pocket.input.keys;
  const N = '`1234567890-=qwertyuiop[]\\asdfghjkl;\'zxcvbnm,./'.split('')
    .concat(['del', 'tab', 'enter', 'space', 'fn', 'shift', 'ctrl', 'opt', 'alt']);
  const INK = 0x101820ff, SUN = 0xfffb96ff, MINT = 0x05ffa1ff, CYAN = 0x01cdfeff;
  let p = 0, r = 0, most = 0, f = 0, row = [];
  const c = pocket.capabilities.get('input.keys');
  console.log('KEYTEST_READY cap=' + c.supported + '/' + c.available + ' keys=' + N.length);

  function scene(tx) {
    tx.background(INK);
    tx.text({bounds: [6, 4, 234, 18], text: 'KEY TEST   ` QUITS', font: 'body', color: SUN});
    row = [24, 46, 68, 90, 112].map(function (y, i) {
      return tx.text({bounds: [6, y, 234, y + 16], text: '-', capacity: 160, font: 'body',
        color: i ? CYAN : MINT});
    });
  }
  function put(tx, d, ev) {
    row[0].setText(tx, d.length ? d.join(' ') : '-');
    row[1].setText(tx, 'NOW ' + d.length + '  MAX ' + most);
    row[2].setText(tx, 'PRESS ' + p + '  RELEASE ' + r);
    row[3].setText(tx, ev.slice(0, 8).join(' ') || '-');
    row[4].setText(tx, 'FRAME ' + f);
  }
  // One replace: a patch before it is presented is refused (BUSY).
  V.replace(function (tx) { scene(tx); put(tx, [], []); });

  globalThis.frame = function () {
    f++;
    const ev = [];
    for (let i = 0; i < N.length; i++) {
      if (K.pressed(N[i])) { p++; ev.push('+' + N[i]); }
      if (K.released(N[i])) { r++; ev.push('-' + N[i]); }
    }
    if (!ev.length) return;
    const d = K.down();
    if (d.length > most) most = d.length;
    console.log('KEYTEST f=' + f + ' ' + ev.join(' ') + ' down=' + d.join(',') +
      ' n=' + d.length + ' max=' + most + ' p=' + p + ' r=' + r);
    // Every press the app saw is either still down or was released.
    if (p - r !== d.length) console.log('KEYTEST_SKEW p=' + p + ' r=' + r + ' n=' + d.length);
    V.patch(function (tx) { put(tx, d, ev); });
  };
})();
