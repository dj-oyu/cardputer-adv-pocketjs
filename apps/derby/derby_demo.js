'use strict';
// The demo, loaded when it first starts (attract() in derby_play.js).
function demo(on, n) {
  if (!on) { [pts, raceNo, pick, stake, A] = bk; notes = []; drop(RUN); drop(['photo']); }
  log('DEMO ' + (on ? 'START ' : 'END ') + [pts, raceNo, pick, stake]);
  if (on) { bk = [pts, raceNo, pick, stake, A]; A = null; raceNo = 1e6 + ++dn; pick = 0; stake = 100; }
  dm = on; dk = ''; idle = n; camT = 0;
  enter('pad');
}
// Favourite, then second favourite, one A/D step a time, then 1.
function pilot() {
  dk = '';
  if (scene === 'pad' && t > 45 && !(t % 12)) {
    const g = [0, 1, 2, 3, 4, 5, 6, 7].sort((a, c) => od[a] - od[c] || a - c)[1 - dn % 2];
    dk = g === pick ? '1' : (g - pick + 8) % 8 > 4 ? 'a' : 'd';
  }
}
