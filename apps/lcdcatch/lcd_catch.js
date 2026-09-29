// LCD CATCH: placeholder until the game replaces this file.
(function () {
  const V = pocket.kasane;
  V.replace(function (tx) {
    tx.background(0x101820ff);
    tx.text({bounds: [8, 50, 232, 70], text: 'LCD CATCH', font: 'display', color: 0xfffb96ff});
    tx.text({bounds: [8, 80, 232, 96], text: 'COMING SOON', font: 'body', color: 0x01cdfeff});
  });
  globalThis.frame = function () {};
})();
