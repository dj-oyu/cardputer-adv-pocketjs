// Integer instruction latency probe (README.md, docs/perf/int-latency.md).
// A cover with the count of finished cases. The LCD transfer is asynchronous,
// so a case runs only every other frame: the frame between lets the last
// picture's transfer finish before the next measurement starts.
'use strict';
const D = pocket.shikiDebug, V = pocket.kasane, total = D.benchCount();
const INK = 0x101820ff, SUN = 0xfffb96ff, MINT = 0x05ffa1ff, DIM = 0x2a3a4aff;
const BAR = [8, 72, 232, 84];
let i = 0, f = 0, done = false, count, bar, note;
V.replace(function (tx) {
  tx.background(INK);
  tx.text({bounds: [8, 10, 232, 26], text: 'CYCLE BENCH', font: 'body', color: SUN});
  note = tx.text({bounds: [8, 32, 232, 46], text: 'MEASURING - HANDS OFF', capacity: 32, font: 'caption', color: MINT});
  tx.rect({bounds: BAR, color: DIM});
  bar = tx.rect({bounds: [BAR[0], BAR[1], BAR[0] + 1, BAR[3]], clip: BAR, color: MINT});
  count = tx.text({bounds: [8, 92, 232, 108], text: '0 / ' + total + ' CASES', capacity: 32, font: 'body', color: SUN});
});
function show() {
  try {
    V.patch(function (tx) {
      count.setText(tx, i + ' / ' + total + ' CASES');
      bar.setRect(tx, [BAR[0], BAR[1], BAR[0] + Math.max(1, Math.round((BAR[2] - BAR[0]) * i / total)), BAR[3]]);
      if (done) note.setText(tx, 'DONE');
    });
  } catch (e) { /* the last picture is not presented yet: the next case's patch shows the count */ }
}
console.log('CB BEGIN ' + total);
globalThis.frame = function () {
  // Odd frames only rest: the picture patched after the last case is sent
  // while nothing is being measured.
  if (done || ++f & 1) return;
  const line = D.bench(i);
  if (line === null) { done = true; console.log('CB END ' + i); show(); return; }
  i++;
  console.log(line);
  show();
};
