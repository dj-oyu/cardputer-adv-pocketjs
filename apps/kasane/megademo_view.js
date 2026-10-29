// MEGADEMO chunk 4: the pocket.kasane view's tables (room, HUD, story text,
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
// Act I's HUD over the cockpit image (megademo_brief.js): the RX lamp,
// bezels round the waves, the spectrum, the scope and two readouts; RDB:
// the readouts (speed, height, a code), numbers no one has to read.
const HUDI = '6 5 32 16 d93436ff 2,9 6 31 15 ffffffff RX,3 104 121 134 2a6a5eff -1,125 104 189 134 2a6a5eff -1,\
192 104 237 134 2a6a5eff -1,1 56 23 67 06121aff,215 56 239 67 06121aff',
  RDB = [[3, 57, 23, 66], [217, 57, 239, 66], [36, 6, 66, 15]];
// Chapters: title, room glow, card ink. Scene -> chapter is CHAP (WAKE, the
// prologue, has no title).
const CHAP = [0, 0, 0, 1, 2, 3, 4, 5], TITLE = 'I BRIEFING|II CROSSING|III LETHE|IV CORE|V SIGNAL LOST|',
  GLOW = [0x1e3a62ff, 0x3a1856ff, 0x12402aff, 0x58301aff, 0x101428ff, 0x0c0a1cff],
  INK = [0, 0xf848ffff, 0x18f038ff, 0xff6838ff];
// Narration per scene: "bounds|first frame|line|..." (the briefing has none).
const SAY = [, , , '6 122 234 133|10|CROSSING THE RIVER|56|DEEPER|92|INTO LETHE',
'150 121 236 132|10|TAMA: ONLINE|40|EVADE!|72|TARGET LOCKED',
'6 106 234 117|10|THE SOURCE OF THE SIGNAL|56|THE SIGNAL OPENS ITS EYE|84|SIGNAL CRITICAL|100|IT HOLDS ITS BREATH|115|AND BREAKS'];
// Tama sits on the cushion.
const HOME = [196, 70, 228, 102];
// The sortie number goes up every night (each WAKE); the memory of the
// briefing clouds with it: LETHE's letters turn to digits, more each night.
let sortie = 741;
const TALLY = 20;
const GB = {E: '3', I: '1', O: '0', A: '4', S: '5', T: '7'};
const gb = s => s.replace(/[EIOAST]/g, (c, i) =>
  (M.sin(i * 12.9898 + sortie * 78.233) * 43758.5453 % 1 + 1) % 1 < M.min(.5, (sortie - 742) * .08) ? GB[c] : c);
