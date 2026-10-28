// MEGADEMO: NIGHT SIGNAL. A room, a cat, a monitor; the story plays on the
// screen: Act I news (3x16 frames, held twice), TWIST, ZENITH, CORE, LOST.
// Story: docs/apps/megademo-story.md; limits: docs/kasane/megademo-limit-scenes.md
// The app is in chunks (apps/kasane/chunks.txt), loaded here one parse at a
// time. This entry names pocket.kasane, so the session reserves its arena.
'use strict';
pocket.app.load('act1');
pocket.app.load('act2');
if (pocket.kasane && pocket.kasane.procedural) {
  pocket.app.load('view');
  pocket.app.load('loop');
}
