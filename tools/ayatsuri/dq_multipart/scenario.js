"use strict";
const M=pocket.ayatsuri.modelOpen(fixture),stats={},beforeStats={};let S;
const inputs=multiCases.map(c=>[new Uint16Array(c.D),new Float32Array(c.G),new Float32Array(c.IB),new Float32Array(c.V),new Uint16Array(c.J),new Uint16Array(c.W)]);
const [dd,gg,ii,vv,jj,ww]=inputs[0],zu=new Uint16Array(0),zf=new Float32Array(0);
function check(x){if(!x)throw Error('multipart assertion');}
function rejects(f){let threw=false;try{f()}catch(e){threw=true}check(threw);}
function fresh(){if(S)S.dispose();S=AyatsuriNative.scene({model:M,camera:{position:[0,0,400],target:[0,0,0],zoom:20,center:[120,67]},light:{direction:[0,0,1]}});}
function draw(){P.beginMeshFrame(17,2048,200000,1);S.render();P.commit();}
function evaluate(){S.evaluateSkinsRigidDQ(0,dd,gg,ii,vv,jj,ww);}
function step(n){S.evaluateSkinsRigidDQ(n,...inputs[n]);draw();}
fresh();
function invalid(){
 for(const [a,k,value] of [[dd,3,1],[jj,18,2],[ww,16,1],[gg,24,2],[ii,24,2],[dd,4,3]]){
  S.stats(beforeStats);const old=a[k];a[k]=value;rejects(evaluate);a[k]=old;check(!S.ready&&!S.busy);S.stats(stats);check(stats.evalCalls===beforeStats.evalCalls);evaluate();
 }
 rejects(()=>S.evaluateSkinsRigidDQ(NaN,dd,gg,ii,vv,jj,ww));check(S.ready&&!S.busy);
 Object.defineProperty(stats,'nodes',{configurable:true,set(){rejects(evaluate);rejects(()=>S.dispose());throw Error('busy');}});rejects(()=>S.stats(stats));delete stats.nodes;check(!S.busy);evaluate();
}
function empty(){S.evaluateSkinsRigidDQ(0,zu,zf,zf,zf,zu,zu);draw();}
function projectFail(){S.camera({zoom:3e38});rejects(evaluate);check(!S.ready&&!S.busy);rejects(draw);S.camera({zoom:20});}
const sd=new Uint16Array([1,4,2]),sg=gg.subarray(0,24),si=ii.subarray(0,24),sv=vv.subarray(0,24),sj=jj.subarray(0,16),sw=ww.subarray(0,16);
let oldCapacity=0;
function warmSmall(){S.evaluateSkinsRigidDQ(0,sd,sg,si,sv,sj,sw);S.stats(stats);oldCapacity=stats.skinWorkBytes;check(oldCapacity>352);draw();}
function failed(initial){check(!S.ready&&!S.busy);rejects(draw);S.stats(stats);check(stats.skinWorkBytes===(initial?0:oldCapacity));}
function grown(){check(S.ready&&!S.busy);S.stats(stats);check(stats.skinWorkBytes>oldCapacity);draw();}
function poison(){for(const row of inputs)for(const a of row)a.fill(0);S.handle(1).visible(false);S.dispose();M.dispose();}
