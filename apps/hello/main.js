// Input advances domain state independently of display acknowledgement.
(function () {
  const view = pocket.kasane;
  const state = {count: 0};
  const scene = view.createScene({
    build: function (tx, state) {
      tx.background(0x071425ff);
      tx.text({bounds: [16, 12, 226, 22], font: 'caption', color: 0x69cdeeff,
        text: 'KASANE / JAVASCRIPT'});
      tx.text({bounds: [16, 34, 236, 52], font: 'display', color: 0xf0f8ffff,
        text: 'Hello, World!'});
      tx.roundRect({bounds: [16, 62, 224, 100], color: 0x12334aff, radius: 6});
      const counter = tx.text({bounds: [28, 77, 212, 89], font: 'caption', color: 0x8ef0c4ff,
        text: 'KEY PRESSES: ' + state.count, capacity: 32});
      tx.text({bounds: [16, 117, 236, 127], font: 'caption', color: 0xa9bacaff,
        text: 'ENTER +1    ESC HOME'});
      return {counter: counter};
    },
    patch: function (tx, refs, state) {
      refs.counter.setText(tx, 'KEY PRESSES: ' + state.count);
    }
  });
  pocket.input.onAction(function (event) {
    if (event.action === 'accept' && event.phase === 'press') {
      state.count++;
      scene.invalidate();
      console.log('HELLO_COUNT ' + state.count);
    }
  });
  globalThis.frame = function () { scene.flush(state); };
  scene.flush(state);
  console.log('HELLO_READY');
})();
