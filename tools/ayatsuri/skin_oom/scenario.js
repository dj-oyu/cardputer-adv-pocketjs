"use strict";
const M = pocket.ayatsuri.modelOpen(fixture), stats = {};
const I = [1,0,0,0,0,1,0,0,0,0,1,0];
let S, mode = 0;
function check(value) { if (!value) throw Error('skin allocation site assertion'); }
function rejects(fn) { let threw = false; try { fn(); } catch (_) { threw = true; } check(threw); }
function input(parts, joints, height) {
  const d=[], g=[], v=[], j=[], w=[];
  for (let p=0;p<parts;p++) {
    d.push(p+1,4,joints);
    for (let k=0;k<joints;k++) g.push(...I);
    const x=p?3:-3;
    for (const [px,py] of [[x-1,-height],[x+1,-height],[x+1,height],[x-1,height]]) {
      v.push(px,py,0,0,0,1); j.push(0,joints-1,0,0); w.push(32768,32767,0,0);
    }
  }
  return [new Uint16Array(d),new Float32Array(g),new Float32Array(g),
    new Float32Array(v),new Uint16Array(j),new Uint16Array(w)];
}
const singleSmall=input(1,1,1), singleLarge=input(1,2,.25);
const batchSmall=input(1,1,1), batchLarge=input(2,2,.25);
function fresh(n) {
  if (S) S.dispose(); mode=n;
  S=AyatsuriNative.scene({model:M,camera:{position:[0,0,400],target:[0,0,0],zoom:20,center:[120,67]},light:{direction:[0,0,1]}});
}
function evaluate(a) {
  if (mode<2) {
    if (mode===0) S.evaluateSkin(0,1,...a.slice(1));
    else S.evaluateSkinRigidDQ(0,1,...a.slice(1));
  } else {
    if (mode===2) S.evaluateSkins(0,...a);
    else S.evaluateSkinsRigidDQ(0,...a);
  }
}
function draw(clear=17) { P.beginMeshFrame(clear,2048,200000,1); S.render(); P.commit(); }
function cold() { S.evaluate(0); draw(); }
function warm() { evaluate(mode<2?singleSmall:batchSmall); draw(); }
function attempt() { evaluate(mode<2?singleLarge:batchLarge); }
function dirty() { S.handle(1).place({position:[1,0,0]}); }
function failed(bytes) {
  check(!S.ready&&!S.busy); rejects(()=>S.render());
  S.stats(stats); check(stats.skinWorkBytes===bytes);
}
function dirtyApplied() { check(S.mask[1]===0); }
function succeeded(bytes) {
  check(S.ready&&!S.busy); S.stats(stats); check(stats.skinWorkBytes===bytes); draw();
}
function finish() { S.dispose(); M.dispose(); }
