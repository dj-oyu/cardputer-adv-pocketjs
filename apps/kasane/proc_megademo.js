// MEGADEMO: ASTRAL FIGHTER. A room, Tama the cat, a TV. Each night the
// viewer wakes (WAKE), is briefed (Act I, 3x16 frames held twice), leaves the
// body (TWIST), fights over LETHE (ZENITH), breaks the core (CORE) and the
// signal is lost (LOST); then wakes again, one sortie later.
// Story: docs/apps/megademo-story.md; limits: docs/kasane/megademo-limit-scenes.md
// The app is in chunks (apps/kasane/chunks.txt), loaded here one parse at a
// time. This entry names pocket.kasane, so the session reserves its arena.
'use strict';
pocket.app.load('act1');
pocket.app.load('act2');
pocket.app.load('wake');
if (pocket.kasane && pocket.kasane.procedural) {
  pocket.app.load('view');
  pocket.app.load('set');
  pocket.app.load('loop');
}
