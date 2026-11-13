"use strict";
const M=pocket.ayatsuri.modelOpen(fixture),S=AyatsuriNative.scene({model:M,camera:{position:[0,0,400],target:[0,0,0],zoom:20,center:[120,67]},light:{direction:[0,0,1]}});
const values=[0,.5,.979999,.97999999,.98,.980001,1],handles=[S.handle(1),S.handle(2)];
let buffers=[];
function geometry(controls){
 if(!Array.isArray(controls)||controls.length!==3)throw Error('control array');
 const captured=[];
 for(let i=0;i<3;i++){
  if(!Object.prototype.hasOwnProperty.call(controls,i))throw Error('missing control');
  const v=controls[i];if(typeof v!=='number'||!Number.isFinite(v)||v<0||v>1)throw Error('control');
  captured.push(v);
 }
 const [b,l,r]=captured;
 const levels=[Math.max(b,l),Math.max(b,r)],ds=[],vs=[],gs=[],js=[],ws=[];
 for(let s=0;s<2;s++){const visible=levels[s]<.98;if(!visible)continue;
  ds.push(s+1,4,1);gs.push(1,0,0,0,0,1,0,0,0,0,1,0);
  const x=s?3:-3,a=Math.fround(1-levels[s]);
  for(const [px,py] of [[x-1,-a],[x+1,-a],[x+1,a],[x-1,a]]){vs.push(px,py,0,0,0,1);js.push(0,0,0,0);ws.push(65535,0,0,0);}
 }
 buffers=[new Uint16Array(ds),new Float32Array(gs),new Float32Array(gs),new Float32Array(vs),new Uint16Array(js),new Uint16Array(ws)];
 for(let s=0;s<2;s++)handles[s].visible(levels[s]<.98);
 S.evaluateSkins(0,...buffers);P.beginMeshFrame(17,2048,200000,1);S.render();P.commit();
}
function step(n){geometry([values[Math.floor(n/49)],values[Math.floor(n/7)%7],values[n%7]]);}
function invalid(){
 const before=JSON.stringify(Array.from(S.mask)),prior=buffers;
 const inputs=[[NaN,0,0],[0,Infinity,0],[0,0,-1],[2,0,0],[0,true,0],Array(3),[,0,0],[0,,0],[0,0,,],null,new Float32Array(3)];
 for(let i=0;i<3;i++){
  const throwing=[0,0,0];Object.defineProperty(throwing,i,{get(){throw Error('getter');}});inputs.push(throwing);
 }
 for(const a of inputs){let threw=false;try{geometry(a)}catch(e){threw=true}
  if(!threw||!S.ready||prior!==buffers||before!==JSON.stringify(Array.from(S.mask)))throw Error('invalid changed Scene');
 }
}
function getters(){
 const reads=[0,0,0],controls=[0,0,0];
 for(let i=0;i<3;i++)Object.defineProperty(controls,i,{get(){reads[i]++;if(!S.ready)throw Error('early Scene mutation');return 0;}});
 geometry(controls);if(reads.some(n=>n!==1))throw Error('repeat read');
}
function stopPending(){for(const a of buffers)a.fill(0);handles[0].visible(false);handles[1].visible(false);S.dispose();M.dispose();}
