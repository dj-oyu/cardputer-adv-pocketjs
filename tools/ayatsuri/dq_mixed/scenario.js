"use strict";
const M=pocket.ayatsuri.modelOpen(fixture),stats={};let S;
function check(x){if(!x)throw Error('mixed assertion');}
function rejects(f){let ok=false;try{f()}catch(e){ok=true}check(ok);}
function fresh(){if(S)S.dispose();S=AyatsuriNative.scene({model:M,camera:{position:[0,0,400],target:[0,0,0],zoom:20,center:[120,67]},light:{direction:[0,0,1]}});S.handle(2).visible(false);}
const inputs=dqCases.map(c=>[new Float32Array(c.G),new Float32Array(c.IB),new Float32Array(c.V),new Uint16Array(c.J),new Uint16Array(c.W)]);
function draw(){P.beginMeshFrame(17,2048,200000,1);S.render();P.commit();}
function step(n){S.evaluateSkinRigidDQ(n,1,...inputs[n]);draw();}
fresh();
const oneG=inputs[0][0].slice(0,12),oneI=inputs[0][1].slice(0,12),oneJ=new Uint16Array(16),oneW=new Uint16Array(16);for(let i=0;i<4;i++)oneW[i*4]=65535;
function warm(){S.evaluateSkinRigidDQ(0,1,oneG,oneI,inputs[0][2],oneJ,oneW);S.stats(stats);check(stats.skinWorkBytes===304);draw();}
const gg=inputs[0][0],ii=inputs[0][1],vv=inputs[0][2],jj=inputs[0][3],ww=inputs[0][4];
function grow(){S.evaluateSkinRigidDQ(0,1,gg,ii,vv,jj,ww);}
function growthFailed(){check(!S.ready&&!S.busy);rejects(draw);S.stats(stats);check(stats.skinWorkBytes===304);}
function grown(){check(S.ready&&!S.busy);S.stats(stats);check(stats.skinWorkBytes===352);draw();}
function poison(){for(const row of inputs)for(const a of row)a.fill(0);S.dispose();M.dispose();}
