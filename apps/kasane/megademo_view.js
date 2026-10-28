// MEGADEMO chunk 3: the pocket.kasane view's tables (room, HUD, story text,
// WAKE's face). Loaded only on the device (or a host with pocket.kasane).
'use strict';
const V = pocket.kasane, H = V.procedural, G = V.grid;
const res0 = H.resource();
// The settled monitor: a registered image-to-image resize (bilinear).
// ZENITH's radar thumbnail and munching squares went for the story's
// heap (typed arrays alone keep ~1.3 KB): docs/apps/megademo-story.md
const monRes = G.resource(G.registerResizeSource({source: res0, width: 112, height: 63}));
const pets = V.resource('pets');
const lives = V.cache.create([[0, 2, 9, 7], [0, 0, 2, 2], [7, 0, 9, 2]].map(b => ({bounds: b, color: 0x14f53cff})));
const FULL = [0, 0, 240, 135], MON = [64, 20, 176, 83], ZOOM = 44;
// Loader: the next scene registers from LOAD[0] frames before the switch,
// LOAD[1] plans a frame, while the internal heap has LOAD[2] bytes free
// (a registration dips it by up to 14 KB on the device); the rest
// registers over the scene's first frames, which skip unloaded plans.
const LOAD = [16, 1, 22528], MEM = pocket.memory;
// The room (built only while the camera is off the screen) and the news
// HUD: "x0 y0 x1 y1 rrggbbaa radius|-stroke|opacity" (a text when 5 has
// letters). Window, moon, stars, desk, monitor, keyboard, cushion. One
// literal each: a '+' keeps the pieces and the joined copy.
const ROOM = '10 14 46 58 2a2440ff,12 16 44 56 0a1230ff,33 20 40 27 f0e8c8ff 3,31 19 38 26 0a1230ff 3,\
16 22 17 23 ffffffff,22 41 23 42 ffffffff,39 46 40 47 ffffffff,27 16 29 56 2a2440ff,12 35 44 37 2a2440ff,\
0 100 240 135 4a3020ff,0 100 240 102 6a4830ff,104 94 136 101 b0a488ff,52 10 188 96 d8ccb0ff 8,\
60 16 180 87 6a6050ff -2,150 89 176 92 6a6050ff,70 104 170 113 c8bca0ff 2,74 106 166 111 a89c80ff,\
192 99 232 105 8a3050ff 3';
const TV = '0 110 240 135 061521ff 0 210,0 110 240 112 45cddaff,6 5 32 16 d93436ff 2,\
9 6 31 15 ffffffff RX,36 6 120 15 d7f4ffff ASTRAL_COMMAND';
// Chapters: title, room glow, card ink. Scene -> chapter is CHAP (WAKE, the
// prologue, has no title).
const CHAP = [0, 0, 0, 1, 2, 3, 4, 5], TITLE = 'I BRIEFING|II CROSSING|III LETHE|IV CORE|V SIGNAL LOST|',
  GLOW = [0x1e3a62ff, 0x3a1856ff, 0x12402aff, 0x58301aff, 0x101428ff, 0x0c0a1cff],
  INK = [0, 0xf848ffff, 0x18f038ff, 0xff6838ff];
// Narration per scene: "bounds|first frame|line|..."; the briefing (two
// lines a beat; ~ is the sortie number, 742 on the first night).
const SAY = [, , , '6 122 234 133|10|LEAVING THE BODY|56|DEEPER|92|INTO LETHE',
'150 121 236 132|10|TAMA: ONLINE|40|EVADE!|72|TARGET LOCKED',
'6 106 234 117|10|THE SOURCE OF THE SIGNAL|56|THE SIGNAL OPENS ITS EYE|84|SIGNAL CRITICAL|100|IT HOLDS ITS BREATH|115|AND BREAKS',
'94 62 160 73|20|NO SIGNAL'];
const NEWS = 'INCOMING ORDER  SORTIE ~|RECEIVER: ASTRAL FIGHTER|TARGET: LETHE|ROUTE SYNC COMPLETE|\
PILOT CONFIRMED|LEAVE THE BODY. CROSS THE RIVER|P1L0T C0NF1RM3D';
// Tama sits on the cushion; the body (the viewer's own) sleeps over the
// keyboard while the mind is away: shoulders, then the head.
const HOME = [196, 70, 228, 102], BODY = [[90, 104, 150, 117, 5, 0x56506eff], [112, 98, 130, 112, 7, 0x2c2638ff]];
// WAKE's face, over act2's ears. A roundRect's radius is at most 8, so
// the round parts are horizontal bands (an ellipse, shaded down its rows)
// in cache templates: one instance ref moves a whole part. Head with
// cheeks; open eyes (iris, slit pupil, highlight); closed eyes (a line).
const mix = (a, b, u) => [24, 16, 8, 0].reduce((s, k) =>
  s + (rnd(((a >>> k) & 255) * (1 - u) + ((b >>> k) & 255) * u) << k >>> 0), 0);
function bands(cx, cy, rx, ry, n, c0, c1) {
  const r = [];
  for (let i = 0; i < n; ++i) {
    const y0 = cy - ry + 2 * ry * i / n, y1 = y0 + 2 * ry / n, w = rx * M.sqrt(1 - ((y0 + y1) / 2 - cy) ** 2 / ry / ry);
    r.push({bounds: [rnd(cx - w), rnd(y0), rnd(cx + w), rnd(y1)], color: mix(c0, c1, i / (n - 1))});
  }
  return r;
}
const IRIS = [0xb8e070ff, 0x6ea838ff], CREAM = 0xfdebd2ff, LID = 0x7a4a2aff;
// Nose and a small mouth (plain refs: the 48 rect commands shared by every
// template are used up; lives has 3).
const MOUTH = [[162, 89, 178, 98, 4, 0xf0889aff], [162, 100, 170, 103, 1, LID], [170, 100, 178, 103, 1, LID]];
// Built by the first frame(), not while the app evaluates (its arrays would
// add to the compile's peak) nor inside a replace (cache.create is BUSY).
let FACE = null;
const face = () => [V.cache.create(bands(170, 86, 94, 74, 20, 0xf8c884ff, 0xd0803eff)
    .concat(bands(157, 108, 19, 12, 3, CREAM, CREAM), bands(183, 108, 19, 12, 3, CREAM, CREAM))),
  V.cache.create(bands(135, 77, 15, 16, 6, IRIS[0], IRIS[1]).concat(bands(205, 75, 15, 16, 6, IRIS[0], IRIS[1]),
    [[132, 64, 138, 90], [202, 62, 208, 88]].map(b => ({bounds: b, color: 0x241a14ff})),
    [[125, 67, 131, 73], [195, 65, 201, 71]].map(b => ({bounds: b, color: 0xffffffff})))),
  V.cache.create([[120, 76, 150, 79], [190, 74, 220, 77]].map(b => ({bounds: b, color: LID})))];
// The sortie number goes up every night (each WAKE); the memory of the
// briefing clouds with it: letters turn to digits, more each night.
let sortie = 741;
const GB = {E: '3', I: '1', O: '0', A: '4', S: '5', T: '7'};
const gb = s => s.replace(/[EIOAST]/g, (c, i) =>
  (M.sin(i * 12.9898 + sortie * 78.233) * 43758.5453 % 1 + 1) % 1 < M.min(.5, (sortie - 742) * .08) ? GB[c] : c);
