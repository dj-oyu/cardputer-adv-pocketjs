'use strict';
const BC=new Float32Array([0,0,400,0,0,0,1,120,67]),BL=new Float32Array([0,0,1,1,1,1]);
const BD=new Uint16Array([1,6,2,2,4,1]),BG=new Float32Array(36),BI=new Float32Array(36);
const BV=new Float32Array(60),BJ=new Uint16Array(40),BW=new Uint16Array(40),BS={},MS={};
const P0=new Float32Array([15,0,0,0,0,0,1,1,1]),P1=new Float32Array([0,0,0,0,0,0,1,1,1]);
const child0=BG.subarray(12,24),child1=BG.subarray(24),D0=BD.subarray(0,3),G0=BG.subarray(0,24),I0=BI.subarray(0,24);
const V0=BV.subarray(0,36),J0=BJ.subarray(0,24),W0=BW.subarray(0,24),Zf=new Float32Array(0),Zu=new Uint16Array(0);
let M,A,B;
globalThis.batchGlobals=BG;globalThis.batchBind=BI;globalThis.cycleBatch=null;
function check(ok){if(!ok)throw Error('batch assertion');}
function rejects(fn){let fail=false;try{fn();}catch(e){fail=true;}check(fail);}
function limitRejects(fn){let fail=false;try{fn();}catch(e){fail=e.code==='LIMIT_EXCEEDED';}check(fail);}
function ident(a){a.fill(0);for(let j=0;j<a.length;j+=12)a[j]=a[j+5]=a[j+10]=1;}
function batchSetup(){
  ident(BG);ident(BI);BI[15]=-15;
  for(let v=0;v<6;v++){
    BV[v*6]=(v%3)*30-30;BV[v*6+1]=v<3?-15:15;BV[v*6+5]=1;BJ[v*4+1]=1;
    BW[v*4+1]=v%3===0?0:v%3===1?21845:65535;BW[v*4]=65535-BW[v*4+1];
  }
  const back=[-35,-20,35,-20,35,20,-35,20];
  for(let v=0;v<4;v++){BV[(v+6)*6]=back[v*2];BV[(v+6)*6+1]=back[v*2+1];BV[(v+6)*6+5]=1;BW[(v+6)*4]=65535;}
  M=pocket.ayatsuri.modelOpen(fixture);A=M.instance();B=M.instance();A.evaluate(BC,BL);B.evaluate(BC,BL);
  A.stats(BS);check(BS.skinWorkBytes===0);
}
function batchPose(frame){
  P0[1]=8*Math.sin(frame*.07);P0[2]=12*Math.sin(frame*.13);P0[4]=.7*Math.sin(frame*.11);
  P1[0]=7*Math.sin(frame*.09);P1[1]=3*Math.cos(frame*.08);P1[2]=9*Math.cos(frame*.12);P1[5]=.15*Math.sin(frame*.06);
  pocket.ayatsuri.compose(P0,null,child0);pocket.ayatsuri.compose(P1,null,child1);
}
function batchEvaluate(frame){batchPose(frame);A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW);}
function batchDraw(){P.beginMeshFrame(17,2048,200000,1);A.submit();P.commit();}
function batchDrawB(){P.beginMeshFrame(17,2048,200000,1);B.submit();P.commit();}
function batchInspect(bytes){A.stats(BS);check(BS.evaluated&&BS.skinWorkBytes===bytes);}
function batchTryB(){B.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW);}
function batchTryA(){A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW);}
function batchFreshB(){B=M.instance();B.evaluate(BC,BL);}
function batchDisposeB(){B.dispose();B=null;}
function batchInspectB(ok){B.stats(BS);check(BS.evaluated&&BS.skinWorkBytes===(ok?batchBytes:0));}
function batchSingleA(){A.evaluateSkin(BC,BL,1,G0,I0,V0,J0,W0);}
function batchLateInvalid(){
  const old=BW[36];BW[36]=0;rejects(()=>A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW));BW[36]=old;batchDraw();
}
function batchInvalid(){
  const before={};A.stats(before);const args=[BC,BL,BD,BG,BI,BV,BJ,BW];
  for(let i=0;i<8;i++){const value=args[i];args[i]=null;rejects(()=>A.evaluateSkins(...args));args[i]=value;}
  let getter=false;rejects(()=>A.evaluateSkins(BC,BL,{get length(){getter=true;return 6;}},BG,BI,BV,BJ,BW));check(!getter);
  rejects(()=>A.evaluateSkins(BC,BL,new Uint32Array(BD),BG,BI,BV,BJ,BW));
  rejects(()=>A.evaluateSkins(BC,BL,BD.subarray(0,5),BG,BI,BV,BJ,BW));
  rejects(()=>A.evaluateSkins(BC,BL,BD,BG.subarray(0,35),BI,BV,BJ,BW));
  rejects(()=>A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW.subarray(0,39)));
  for(const [index,value] of [[3,1],[3,3],[4,33],[4,0],[5,0],[5,257]]){
    const old=BD[index];BD[index]=value;rejects(()=>A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW));BD[index]=old;
  }
  let old=BW[36];BW[36]=0;rejects(()=>A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW));BW[36]=old;
  old=BJ[39];BJ[39]=1;rejects(()=>A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW));BJ[39]=old;
  old=BV[59];BV[59]=0;rejects(()=>A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW));BV[59]=old;
  old=BV[54];BV[54]=NaN;rejects(()=>A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW));BV[54]=old;
  old=BI[35];BI[35]=Infinity;rejects(()=>A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW));BI[35]=old;
  const saved=BG.slice(24);BG.fill(0,24);rejects(()=>A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW));BG.set(saved,24);
  const detached=BD.slice();detachBuffer(detached.buffer);rejects(()=>A.evaluateSkins(BC,BL,detached,BG,BI,BV,BJ,BW));
  const shared=new Uint16Array(new SharedArrayBuffer(BD.byteLength));shared.set(BD);rejects(()=>A.evaluateSkins(BC,BL,shared,BG,BI,BV,BJ,BW));
  rejects(()=>A.evaluateSkins(BC,BL,new Uint16Array(foreignD),BG,BI,BV,BJ,BW));
  rejects(()=>A.evaluateSkins(BC,BL,BD,new Float32Array(foreignBG),BI,BV,BJ,BW));
  A.stats(BS);check(BS.evaluated&&BS.evalCalls===before.evalCalls&&BS.skinWorkBytes===before.skinWorkBytes&&BS.geometryUpdates===before.geometryUpdates);
  const bad=BC.slice();bad[6]=1000;rejects(()=>A.evaluateSkins(bad,BL,BD,BG,BI,BV,BJ,BW));rejects(()=>A.submit());
  A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW);
  for(const bytes of [holes,sided]){
    const model=pocket.ayatsuri.modelOpen(bytes),actor=model.instance();actor.evaluate(BC,BL);
    actor.stats(BS);check(BS.skinWorkBytes===0);rejects(()=>actor.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW));
    actor.stats(BS);check(BS.evaluated&&BS.skinWorkBytes===0);actor.dispose();model.dispose();
  }
  const immutable=BD.slice();freezeBuffer(immutable.buffer);A.evaluateSkins(BC,BL,immutable,BG,BI,BV,BJ,BW);
  Object.defineProperty(BD,'buffer',{get(){throw Error('descriptor getter');}});
  A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW);
  Object.defineProperty(BS,'nodes',{configurable:true,set(){rejects(()=>A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW));rejects(()=>A.dispose());throw Error('setter');}});
  rejects(()=>A.stats(BS));delete BS.nodes;A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW);
}
function batchAliases(){
  const pool=new Float32Array(48);ident(pool);
  A.evaluateSkins(BC,BL,BD,pool.subarray(0,36),pool.subarray(12,48),BV,BJ,BW);
  check(pool.every((x,i)=>x===((i%12===0||i%12===5||i%12===10)?1:0)));
  const reversed=new Uint16Array([2,4,1,1,6,2]);
  const g=new Float32Array(36),b=new Float32Array(36),v=new Float32Array(60),j=new Uint16Array(40),w=new Uint16Array(40);
  g.set(BG.subarray(24));g.set(G0,12);b.set(BI.subarray(24));b.set(I0,12);
  v.set(BV.subarray(36));v.set(V0,24);j.set(BJ.subarray(24));j.set(J0,16);w.set(BW.subarray(24));w.set(W0,16);
  A.evaluateSkins(BC,BL,reversed,g,b,v,j,w);batchDraw();
}
function batchEmpty(){A.evaluateSkins(BC,BL,Zu,Zf,Zf,Zf,Zu,Zu);batchInspect(batchBytes);batchDraw();}
function batchSubset(){A.evaluateSkins(BC,BL,D0,G0,I0,V0,J0,W0);batchInspect(batchBytes);batchDraw();}
function batchRelease(){
  M.dispose();A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW);batchDraw();
  A.dispose();A.dispose();B.dispose();M.stats(MS);check(MS.released&&MS.instances===0);
  rejects(()=>A.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW));
  A=B=M=null;fixture=holes=sided=null;BV.fill(NaN);BG.fill(NaN);BW.fill(0);
}
function batchLimits(){
  const model=pocket.ayatsuri.modelOpen(limitFixture),actor=model.instance();actor.evaluate(BC,BL);
  const desc=new Uint16Array([1,32,127,2,32,127]),mat=new Float32Array(12*257),vertices=new Float32Array(64*6);
  const indices=new Uint16Array(64*4),weights=new Uint16Array(64*4);ident(mat);
  for(let v=0;v<64;v++){vertices[v*6]=20*Math.cos((v%32)*2*Math.PI/32);vertices[v*6+1]=20*Math.sin((v%32)*2*Math.PI/32);vertices[v*6+5]=1;weights[v*4]=65535;}
  actor.evaluateSkins(BC,BL,desc,mat.subarray(0,12*254),mat.subarray(0,12*254),vertices,indices,weights);
  actor.stats(BS);check(BS.skinWorkBytes===batchNearLimit&&BS.evaluated);
  desc[2]=desc[5]=128;
  limitRejects(()=>actor.evaluateSkins(BC,BL,desc,mat.subarray(0,12*256),mat.subarray(0,12*256),vertices,indices,weights));
  actor.stats(BS);check(BS.skinWorkBytes===batchNearLimit&&BS.evaluated);
  desc[5]=129;limitRejects(()=>actor.evaluateSkins(BC,BL,desc,mat,mat,vertices,indices,weights));
  actor.stats(BS);check(BS.skinWorkBytes===batchNearLimit&&BS.evaluated);
  actor.dispose();model.dispose();limitFixture=null;
  const small=pocket.ayatsuri.modelOpen(cycleFixture),a=small.instance();a.evaluate(BC,BL);
  const jointLimit=new Uint16Array([1,6,129,2,4,128]);
  limitRejects(()=>a.evaluateSkins(BC,BL,jointLimit,mat,mat,vertices.subarray(0,60),indices.subarray(0,40),weights.subarray(0,40)));
  a.stats(BS);check(BS.evaluated&&BS.skinWorkBytes===0);a.dispose();small.dispose();
}
function batchCycle(){
  const model=pocket.ayatsuri.modelOpen(cycleFixture),actor=model.instance();ident(BG);ident(BI);
  for(let v=0;v<10;v++){BV[v*6]=v<6?(v%3)*30-30:(v%2?35:-35);BV[v*6+1]=v%2?15:-15;BV[v*6+2]=BV[v*6+3]=BV[v*6+4]=0;BV[v*6+5]=1;for(let j=0;j<4;j++)BJ[v*4+j]=BW[v*4+j]=0;BW[v*4]=65535;}
  actor.evaluateSkins(BC,BL,BD,BG,BI,BV,BJ,BW);actor.cycle=model;model.cycle=actor;model.dispose();globalThis.cycleBatch=model;
}
