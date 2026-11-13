'use strict';
const M=pocket.ayatsuri.modelOpen(fixture),S=AyatsuriNative.scene({model:M,camera:{center:[120,67]},light:{direction:[0,0,1]}});
const L=S.handle(1),R=S.handle(2);S.handle(0).place({position:[0,0,0]});L.place({position:[-60,0,0]});R.place({position:[60,0,0]});
const values=[0,.5,.979999,.97999999,.98,.980001,1];
function draw(){S.evaluate(0);P.beginMeshFrame(17,2048,200000,1);S.render();P.commit();}
function flags(n){L.visible(!!(n&1));R.visible(!!(n&2));draw();}
function controls(b,l,r){for(const v of [b,l,r])if(typeof v!=='number'||!Number.isFinite(v)||v<0||v>1)throw Error('control');const left=Math.max(b,l)<.98,right=Math.max(b,r)<.98;L.visible(left);R.visible(right);draw();}
function step(n){controls(values[Math.floor(n/49)],values[Math.floor(n/7)%7],values[n%7]);}
function invalid(){for(const a of [[NaN,0,0],[0,Infinity,0],[0,0,-1],[2,0,0],[0,true,0],[0,0,'1']]){let threw=false;try{controls(...a)}catch(e){threw=true}if(!threw||!S.ready)throw Error('invalid');}}
function stopPending(){L.visible(false);R.visible(false);S.dispose();S.dispose();M.dispose();}
