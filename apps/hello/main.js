(function () {
  const view = pocket.kasane.mount('hello');
  let count = 0;
  view.set({counter: 'KEY PRESSES: ' + count});
  pocket.input.onAction(function (event) {
    if (event.action === 'accept' && event.phase === 'press') {
      view.set({counter: 'KEY PRESSES: ' + ++count});
      console.log('HELLO_COUNT ' + count);
    }
  });
  globalThis.frame = function () {};
  console.log('HELLO_READY');
})();
