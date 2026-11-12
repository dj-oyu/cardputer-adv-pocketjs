'use strict';
const C=new Float32Array([0,0,400,0,0,0,1,120,67]),L=new Float32Array([0,0,1,1,1,1]);
const G=new Float32Array(24),IB=new Float32Array(24),V=new Float32Array(36);
const J=new Uint16Array(24),W=new Uint16Array(24),pose=new Float32Array([15,0,0,0,0,0,1,1,1]);
const child=G.subarray(12),bigG=new Float32Array(48),bigIB=new Float32Array(48);
const stats={},modelStats={};
let M,A,B,H,D;
globalThis.skinGlobals=G;globalThis.skinInverseBind=IB;globalThis.cycleModel=null;
function check(ok){if(!ok)throw Error('skin assertion');}
function rejects(fn){let failed=false;try{fn();}catch(e){failed=true;}check(failed);}
function identity(a){a.fill(0);for(let j=0;j<a.length;j+=12)a[j]=a[j+5]=a[j+10]=1;}
function setup(){
  identity(G);identity(IB);IB[15]=-15;identity(bigG);identity(bigIB);
  for(let v=0;v<6;v++){
    V[v*6]=(v%3)*30-30;V[v*6+1]=v<3?-15:15;V[v*6+5]=1;J[v*4+1]=1;
    W[v*4+1]=v%3===0?0:v%3===1?21845:65535;W[v*4]=65535-W[v*4+1];
  }
  M=pocket.ayatsuri.modelOpen(fixture);A=M.instance();B=M.instance();
  H=pocket.ayatsuri.modelOpen(holes);D=pocket.ayatsuri.modelOpen(sided);
  A.evaluate(C,L);B.evaluate(C,L);A.stats(stats);check(stats.skinWorkBytes===0);
}
function poseAt(frame){
  pose[1]=8*Math.sin(frame*.07);pose[2]=12*Math.sin(frame*.13);pose[4]=.7*Math.sin(frame*.11);
  pocket.ayatsuri.compose(pose,null,child);
}
function evaluateOnly(frame){poseAt(frame);A.evaluateSkin(C,L,1,G,IB,V,J,W);}
function drawA(){P.beginMeshFrame(17,2048,200000,1);A.submit();P.commit();}
function drawB(){P.beginMeshFrame(17,2048,200000,1);B.submit();P.commit();}
function tick(frame){evaluateOnly(frame);drawA();}
function attemptSkin(){B.evaluateSkin(C,L,1,G,IB,V,J,W);}
function attemptGrowth(){A.evaluateSkin(C,L,1,bigG,bigIB,V,J,W);}
function freshB(){B=M.instance();B.evaluate(C,L);}
function disposeB(){B.dispose();B=null;}
function inspectB(ok){B.stats(stats);check(stats.evaluated&&stats.skinWorkBytes===(ok?480:0));}
function inspectA(bytes){A.stats(stats);check(stats.evaluated&&stats.skinWorkBytes===bytes);}
function invalidInputs(){
  poseAt(0);A.evaluateSkin(C,L,1,G,IB,V,J,W);
  const before={};A.stats(before);
  const args=[C,L,1,G,IB,V,J,W];
  for(let index=0;index<args.length;index++){
    const saved=args[index];args[index]=null;rejects(()=>A.evaluateSkin(...args));args[index]=saved;
  }
  rejects(()=>A.evaluateSkin(C,L,0,G,IB,V,J,W));
  rejects(()=>A.evaluateSkin(C,L,3,G,IB,V,J,W));
  let coerced=false;rejects(()=>A.evaluateSkin(C,L,{valueOf(){coerced=true;return 1;}},G,IB,V,J,W));check(!coerced);
  rejects(()=>A.evaluateSkin(C,L,1,new Float32Array(12*257),new Float32Array(12*257),V,J,W));
  rejects(()=>A.evaluateSkin(C,L,1,G,IB,V,new Uint32Array(J),W));
  rejects(()=>A.evaluateSkin(C,L,1,G,IB,V.subarray(0,30),J,W));
  rejects(()=>A.evaluateSkin(C,L,1,G,IB,V,J,W.subarray(0,23)));
  let saved=W[0];W[0]=0;rejects(()=>A.evaluateSkin(C,L,1,G,IB,V,J,W));W[0]=saved;
  saved=J[23];J[23]=2;rejects(()=>A.evaluateSkin(C,L,1,G,IB,V,J,W));J[23]=saved;
  saved=V[35];V[35]=0;rejects(()=>A.evaluateSkin(C,L,1,G,IB,V,J,W));V[35]=saved;
  saved=V[30];V[30]=NaN;rejects(()=>A.evaluateSkin(C,L,1,G,IB,V,J,W));V[30]=saved;
  saved=IB[23];IB[23]=Infinity;rejects(()=>A.evaluateSkin(C,L,1,G,IB,V,J,W));IB[23]=saved;
  saved=G[0];G[0]=0;rejects(()=>A.evaluateSkin(C,L,1,G,IB,V,J,W));G[0]=saved;
  G[0]=3.4e38;rejects(()=>A.evaluateSkin(C,L,1,G,IB,V,J,W));G[0]=saved;
  bigG[47]=NaN;rejects(()=>A.evaluateSkin(C,L,1,bigG,bigIB,V,J,W));identity(bigG);
  const growthBefore={};A.stats(growthBefore);saved=W[0];W[0]=0;
  rejects(()=>A.evaluateSkin(C,L,1,bigG,bigIB,V,J,W));W[0]=saved;A.stats(stats);check(stats.skinWorkBytes===growthBefore.skinWorkBytes);
  A.stats(stats);check(stats.evaluated&&stats.evalCalls===before.evalCalls&&stats.geometryUpdates===before.geometryUpdates);
  const badCamera=C.slice();badCamera[6]=1000;rejects(()=>A.evaluateSkin(badCamera,L,1,G,IB,V,J,W));
  rejects(()=>A.submit());A.evaluateSkin(C,L,1,G,IB,V,J,W);
  for(const model of [H,D]){const a=model.instance();a.evaluate(C,L);rejects(()=>a.evaluateSkin(C,L,1,G,IB,V,J,W));a.stats(stats);check(stats.evaluated&&stats.skinWorkBytes===0);a.dispose();}
  const detached=G.slice();detachBuffer(detached.buffer);rejects(()=>A.evaluateSkin(C,L,1,detached,IB,V,J,W));
  const shared=new Float32Array(new SharedArrayBuffer(G.byteLength));shared.set(G);rejects(()=>A.evaluateSkin(C,L,1,shared,IB,V,J,W));
  rejects(()=>A.evaluateSkin(C,L,1,G,IB,V,new Uint16Array(foreignJ),W));
  rejects(()=>A.evaluateSkin(C,L,1,new Float32Array(foreignG),IB,V,J,W));
  const immutable=IB.slice();freezeBuffer(immutable.buffer);A.evaluateSkin(C,L,1,G,immutable,V,J,W);
  Object.defineProperty(G,'buffer',{get(){throw Error('buffer getter must not run');}});
  A.evaluateSkin(C,L,1,G,IB,V,J,W);
  Object.defineProperty(stats,'nodes',{configurable:true,set(){rejects(()=>A.evaluateSkin(C,L,1,G,IB,V,J,W));rejects(()=>A.dispose());throw Error('setter');}});
  rejects(()=>A.stats(stats));delete stats.nodes;A.evaluateSkin(C,L,1,G,IB,V,J,W);
}
function overrideSecond(){
  const vertices=new Float32Array([-27,-20,9,0,0,1,43,-20,9,0,0,1,43,20,9,0,0,1,-27,20,9,0,0,1]);
  const indices=new Uint16Array(16),weights=new Uint16Array(16);
  for(let v=0;v<4;v++)weights[v*4]=65535;
  A.evaluateSkin(C,L,2,G.subarray(0,12),IB.subarray(0,12),vertices,indices,weights);
  inspectA(576);drawA();
}
function closeModel(){M.dispose();A.evaluateSkin(C,L,1,G,IB,V,J,W);M.stats(modelStats);check(modelStats.closed&&!modelStats.released);}
function pendingThenDispose(){
  evaluateOnly(30);drawA();A.dispose();A.dispose();B.dispose();H.dispose();D.dispose();
  M.stats(modelStats);check(modelStats.instances===0&&modelStats.released);
  rejects(()=>A.evaluateSkin(C,L,1,G,IB,V,J,W));rejects(()=>A.submit());
  G.fill(NaN);IB.fill(NaN);V.fill(NaN);J.fill(65535);W.fill(0);
  A=B=M=H=D=null;fixture=holes=sided=null;
}
function implicitRelease(){
  let m=pocket.ayatsuri.modelOpen(cycleFixture),a=m.instance();
  identity(G);identity(IB);IB[15]=-15;
  for(let v=0;v<6;v++){V[v*6]=(v%3)*30-30;V[v*6+1]=v<3?-15:15;V[v*6+2]=V[v*6+3]=V[v*6+4]=0;V[v*6+5]=1;for(let j=0;j<4;j++)J[v*4+j]=W[v*4+j]=0;W[v*4]=65535;}
  a.evaluateSkin(C,L,1,G,IB,V,J,W);a.cycle=m;m.cycle=a;
  m.dispose();globalThis.cycleModel=m;m=null;a=null;
}
