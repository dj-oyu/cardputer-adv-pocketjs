// Overlay app. Why it is written this way: apps/deskclock/README.md
var o = pocket.overlay, t = pocket.time, last = '';
function pad(n) { return n < 10 ? '0' + n : '' + n; }
globalThis.frame = function () {
  var w = t.wall(), face, tag;
  if (w.unixMs === null) { face = '--:--'; tag = 'NO SYNC'; }
  else {
    var d = new Date(w.unixMs);
    face = pad(d.getUTCHours()) + ':' + pad(d.getUTCMinutes());
    tag = w.source === 'network' ? 'UTC' : 'NO SYNC';
  }
  var line = face + tag;
  if (line === last) return;
  last = line;
  o.begin();
  o.rect(0, 0, 96, 22, 6, 12, 26);
  o.rect(0, 0, 96, 1, 62, 220, 208);
  o.text(4, 4, face, 226, 240, 255);
  o.text(50, 12, tag, 99, 214, 221);
};
