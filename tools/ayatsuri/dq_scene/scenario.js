"use strict";
let M=pocket.ayatsuri.modelOpen(fixture),S;
const stats={},modelStats={};let G,IB,V,J,W;
function check(x){if(!x)throw Error('dq scene assertion');}
function rejects(f){let ok=false;try{f()}catch(e){ok=true}check(ok);}
function fresh(){if(S)S.dispose();S=AyatsuriNative.scene({model:M,camera:{position:[0,0,400],target:[0,0,0],zoom:20,center:[120,67]},light:{direction:[0,0,1]}});S.handle(2).visible(false);}
fresh();
const inputs=dqCases.map(c=>[new Float32Array(c.G),new Float32Array(c.IB),new Float32Array(c.V),new Uint16Array(c.J),new Uint16Array(c.W)]);
function evaluateOnly(n){[G,IB,V,J,W]=inputs[n];S.evaluateSkinRigidDQ(n,1,G,IB,V,J,W);}
function draw(){P.beginMeshFrame(17,2048,200000,1);S.render();P.commit();}
function step(n){evaluateOnly(n);draw();}
function bad(){
 const old=G[0];G[0]=2;rejects(()=>S.evaluateSkinRigidDQ(0,1,G,IB,V,J,W));G[0]=old;check(!S.ready&&!S.busy);rejects(draw);evaluateOnly(0);
 const b=IB[0];G[0]=2;IB[0]=.5;rejects(()=>S.evaluateSkinRigidDQ(0,1,G,IB,V,J,W));G[0]=old;IB[0]=b;check(!S.ready&&!S.busy);evaluateOnly(0);
 const w=W[0];W[0]=0;rejects(()=>S.evaluateSkinRigidDQ(0,1,G,IB,V,J,W));W[0]=w;check(!S.ready&&!S.busy);evaluateOnly(0);
 rejects(()=>S.evaluateSkinRigidDQ(NaN,1,G,IB,V,J,W));check(S.ready&&!S.busy);
 Object.defineProperty(stats,'nodes',{configurable:true,set(){rejects(()=>evaluateOnly(0));rejects(()=>S.dispose());throw Error('setter');}});rejects(()=>S.stats(stats));delete stats.nodes;check(!S.busy);evaluateOnly(0);
}
function failed(){check(!S.ready&&!S.busy);rejects(draw);S.stats(stats);check(stats.skinWorkBytes===0);}
function poison(){G.fill(NaN);IB.fill(NaN);V.fill(NaN);J.fill(65535);W.fill(0);M.dispose();S.dispose();S.dispose();M.stats(modelStats);check(modelStats.released);}

function pendingFailure(){S.camera({zoom:3e38});rejects(()=>evaluateOnly(0));check(!S.ready&&!S.busy);rejects(draw);S.camera({zoom:20});}
