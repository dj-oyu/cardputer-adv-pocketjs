// MEGADEMO: ASTRAL FIGHTER. A room, Tama the cat, a TV. Each night the
// viewer wakes (WAKE), is briefed (Act I: a cockpit boots, the route lights,
// the camera cranes over it), crosses the river (TWIST), fights over LETHE (ZENITH), breaks the core (CORE) and the
// signal is lost (LOST); then wakes again, one sortie later.
// Story: docs/apps/megademo-story.md; limits: docs/kasane/megademo-limit-scenes.md
// The app is in chunks (apps/kasane/chunks.txt), loaded here one parse at a
// time. This entry names pocket.kasane, so the session reserves its arena.
'use strict';
// The procedural frames (2 x 10 KB and the VM's workspace) before any chunk
// compiles, while the internal heap is whole: after the compile its largest
// block was 31,744 B and they failed (device, 2026-10-29).
if (pocket.kasane && pocket.kasane.procedural) pocket.kasane.procedural.beginFrame(0);
pocket.app.load('act1');
pocket.app.load('act2');
pocket.app.load('wake');
pocket.app.load('brief');
if (pocket.kasane && pocket.kasane.procedural) {
  pocket.app.load('view');
  pocket.app.load('set');
  pocket.app.load('loop');
}
