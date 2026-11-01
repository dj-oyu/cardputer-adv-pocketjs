// Optional authoring facade; native factories validate/copy and own all state.
'use strict';
const AyatsuriFollowBackend=o=>o&&o.backend||globalThis.pocket.ayatsuri;
globalThis.AyatsuriFollow={
  create(d,o){return AyatsuriFollowBackend(o).followCreate(d);},
  drive(d,o){return AyatsuriFollowBackend(o).driveCreate(d&&d.seed!==undefined?d.seed:42);},
  stepHz:60
};
if(typeof module!=='undefined')module.exports=globalThis.AyatsuriFollow;
