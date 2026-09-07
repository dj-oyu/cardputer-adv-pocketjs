// pocket.ui / pocket.input self-check. Claims: README.md.
(function () {
  var p = pocket, log = console.log, pass = 0, fail = 0;
  var IA = 'INVALID_ARGUMENT', UN = 'UNSUPPORTED';
  var C = n => p.capabilities.get(n);
  var ok = (n, c) => { if (c) pass++; else { fail++; log('UI_FAIL ' + n); } };
  var is = (n, f, w) => {
    var c;
    try { f(); c = 'none'; } catch (e) { c = e.code || String(e); }
    ok(n + ' ' + c, c === w);
  };
  var cap = C('ui.basic'), inp = C('input.action');
  log('UI_CAP ' + JSON.stringify(cap.limits) +
      ' input=' + JSON.stringify(inp.limits));
  ok('caps', cap.supported && inp.supported && !C('input.text').supported);
  var s = p.ui.screen({ background: 0x071425ff });
  var ttl = s.text({ x: 12, y: 22, width: 216, height: 18, font: 'large',
    color: 0xf0f8ffff, text: 'Hello, World!' });
  var T = (y, f, c, t) =>
    s.text({ x: 14, y: y, width: 212, height: 12, font: f, color: c, text: t });
  s.rect({ x: 12, y: 44, width: 216, height: 34, color: 0x12334aff, radius: 5 });
  T(8, 'small', 0x69cdeeff, 'POCKET.UI');
  T(50, 'body', 0x8ef0c4ff, '日本語も出る');
  var n = T(64, 'small', 0xa9bacaff, 'ENTER +0');
  T(120, 'small', 0x8fa6bcff, 'ESC QUITS');
  var mnu = s.list({ x: 12, y: 82, width: 216, height: 24, font: 'small',
    items: [{ id: 'a', label: 'ALPHA', detail: '1' }, { id: 'b', label: 'BRAVO' },
            { id: 'c', label: 'CHARLIE', detail: '3' }], selected: 'a' });
  p.ui.push(s);
  is('compact', () => T(0, 'compact', 255, 'x'), UN);
  is('badfont', () => T(0, 'huge', 255, 'x'), IA);
  is('needtext', () => T(0, 'small', 255), IA);
  is('ascii', () => ttl.setText('日本語'), IA);
  is('long', () => ttl.setText('x'.repeat(1100)), 'LIMIT_EXCEEDED');
  is('noitem', () => mnu.select('zz'), 'NOT_FOUND');
  is('onKey', () => p.input.onKey(() => {}), UN);
  is('textOpen', () => p.input.text.open({}), UN);
  is('badheld', () => p.input.held('nope'), IA);
  ok('notheld', p.input.held('accept') === false);
  var g = T(0, 'small', 255, 'g');
  g.remove(); g.remove();
  is('closed', () => g.setText('x'), 'CLOSED');
  var over = p.ui.screen({ background: 0x1a0f24ff });
  over.text({ x: 16, y: 60, width: 208, height: 12, font: 'small',
    color: 0xffd479ff, text: 'SECOND' });
  p.ui.push(over);
  is('repush', () => p.ui.push(over), IA);
  p.ui.pop();
  is('popped', () => p.ui.push(over), 'CLOSED');
  p.ui.toast('ようこそ', { durationMs: 2000 });
  var made = 0, why = 'none';
  try { for (var i = 0; i < 40; i++) {
    s.rect({x:0, y:0, width:2, height:2, color:255}); made++;
  } } catch (e) { why = e.code; }
  log('UI_BUDGET ' + made + ' ' + why);
  ok('budget ' + why, why === 'OUT_OF_MEMORY' || why === 'LIMIT_EXCEEDED');
  globalThis.frame = function () {};
  var hits = 0;
  p.input.onAction(e => {
    log('UI_ACTION ' + e.action + ' ' + e.phase + ' ' + (e.timeMs | 0));
    if (e.action !== 'accept' || e.phase !== 'press') return;
    hits++;
    n.setText('ENTER +' + hits);
    mnu.select('abc'[hits % 3]);
  });
  log('UI_RESULT pass=' + pass + ' fail=' + fail);
})();
