'use strict';
pocket.app.load('motion');
pocket.app.load('scene');
pocket.app.load('sample');
// Bootstrap supplies the public cold AYNM buffer; release that reference after start.
// Explicit ms clock permits seek/pause without any background task.
globalThis.startSkinSample=blob=>SkinSample.start(blob);
