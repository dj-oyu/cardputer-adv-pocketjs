// MEGADEMO: ASTRAL FIGHTER. A room, Tama the cat, a TV. Each night the
// viewer wakes (WAKE), is briefed (Act I: a cockpit boots, the route lights,
// the camera cranes over it), crosses the river (TWIST), fights over LETHE (ZENITH), breaks the core (CORE) and the
// signal is lost (LOST); then wakes again, one sortie later.
// Story: docs/apps/megademo-story.md; limits: docs/kasane/megademo-limit-scenes.md
// The app is in chunks (apps/kasane/chunks.txt), loaded here one parse at a
// time, the big ones first. This entry names kobako.kasane, so the session
// reserves its arena.
'use strict';
kobako.app.load('act1');
kobako.app.load('act2');
if (kobako.kasane && kobako.kasane.shiki) kobako.app.load('set');
kobako.app.load('brief');
kobako.app.load('wake');
if (kobako.kasane && kobako.kasane.shiki) {
  kobako.app.load('view');
  kobako.app.load('loop');
}
