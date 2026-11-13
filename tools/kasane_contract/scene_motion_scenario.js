// Public host scenario. The application owns time, targets and frame commits.
'use strict';
function check(value){if(!value)throw new Error('scene motion contract');}
function rejects(call){let threw=false;try{call();}catch(e){threw=true;}check(threw);}
var M,S,S2,D,HL,HR,ROOT,track,motionPlayback,mask,poses,colors,opacity;
var camera=new Float32Array([0,0,400,0,0,0,1,120,67]);
var light=new Float32Array([0,0,1,1,1,1]);
var rotation=[0,0,0],position=[0,0,0],rootPosition=[0,0,0];
var rotationPatch={rotation},positionPatch={position},rootPatch={position:rootPosition};
var statsOut={},modelOut={};
var keys=new Float32Array([0,0,0,10,.4,-6,35,-.6,9,80,0,0]);
function openModel(){M=pocket.ayatsuri.modelOpen(fixture);fixture=null;}
function attemptScene(){return AyatsuriNative.scene({model:M});}
function setupScenes(){
  S=AyatsuriNative.scene({model:M,camera:{center:[120,67]},light:{direction:[0,0,1]}});
  S2=AyatsuriNative.scene({model:M,camera:{center:[40,30]},light:{direction:[1,0,1]}});
  D=M.instance();HL=S.node('leftJoint');HR=S.node('rightJoint');ROOT=S.node('root');
  S2.node('leftJoint'); // Resolve bindings before closing the shared owner.
  check(S.node('leftJoint')===HL);
  check(Object.getPrototypeOf(HL)===Object.getPrototypeOf(HR));
  check(S.mask.byteLength+S.poses.byteLength+S.colors.byteLength+S.opacity.byteLength+
    S.cameraState.byteLength+S.lightState.byteLength+S.scratch.byteLength===49*5+132);
  track=AyatsuriMotion.track(keys,{stride:3,timeUnit:'ms'});
  motionPlayback=AyatsuriMotion.performance({track,channels:['time','left','right'],
    waves:[{channel:'lift',amplitude:2,rate:.13,phase:.2,mode:'add'}],
    gates:[{channel:'hide',from:2.5,to:3.25,value:1}]});
  mask=new Uint8Array(5);poses=new Float32Array(45);colors=new Uint32Array(5);opacity=new Float64Array(5);
  // A caller-owned scalar target oracle, independent of the facade's buffers.
  for(let i=0;i<5;i++){poses[9*i+6]=1;poses[9*i+7]=1;poses[9*i+8]=1;}
  M.dispose();M.stats(modelOut);check(modelOut.closed&&modelOut.instances===3&&!modelOut.released);
  check(S.node('leftJoint')===HL);rejects(()=>S.node('right'));
  rejects(attemptScene); // Closing Model stops new actors, not existing ones.
}
function begin(){P.beginMeshFrame(17,4096,200000,1);}
function tickScene(time){
  const p=motionPlayback.evaluate(time);
  rotation[2]=p.left;HL.place(rotationPatch);
  position[0]=32+p.right;position[1]=7;position[2]=-2;HR.place(positionPatch);
  rootPosition[1]=p.lift;ROOT.place(rootPatch);HR.visible(!p.hide);
  S.evaluate(time);begin();S.render();P.commit();
}
function tickDirect(time){
  let a=0,b=0,u=0;
  if(time>=80)a=b=9;
  else if(time>0){while(keys[a+3]<=time)a+=3;b=a+3;u=(time-keys[a])/(keys[b]-keys[a]);}
  const left=a===b?keys[a+1]:keys[a+1]+(keys[b+1]-keys[a+1])*u;
  const right=a===b?keys[a+2]:keys[a+2]+(keys[b+2]-keys[a+2])*u;
  mask.fill(0);mask[0]=1;mask[1]=2;mask[3]=1|((time>=2.5&&time<3.25)?16:8);
  poses[1]=2*Math.sin(time*.13+.2);poses[9+5]=left;
  poses[27]=32+right;poses[28]=7;poses[29]=-2;
  D.targets(mask,poses,colors,opacity);D.evaluate(camera,light);begin();D.submit();P.commit();
}
function invalidSetters(){
  rejects(()=>HL.place({position:[1,2,Infinity],rotation:[0,0,.8]}));
  rejects(()=>S.camera({center:[50,40],zoom:0}));
  rejects(()=>S.light({direction:[0,0,0]}));
  rejects(()=>HL.place({scale:[1,0,1]}));
  rejects(()=>HR.visible(1));rejects(()=>HL.material({color:0xff0000}));
  rejects(()=>S.evaluate(NaN));
  begin();S.render();P.commit(); // Every rejected setter preserves prior ready state.
}
function noHiddenClock(){
  S.stats(statsOut);const before=statsOut.geometryUpdates;
  S.evaluate(1e9);S.stats(statsOut);check(statsOut.geometryUpdates===before);
  begin();S.render();P.commit();
}
function scaleDirect(mode){
  mask.fill(0);mask[1]=4;poses[15]=mode===2?-1:mode===1?1.1:1;poses[16]=mode?.8:1;poses[17]=mode?1.2:1;
  D.targets(mask,poses,colors,opacity);D.evaluate(camera,light);begin();D.submit();P.commit();
}
function scaleScene(mode){
  HL.place({scale:mode===2?[-1,.8,1.2]:mode===1?[1.1,.8,1.2]:[1,1,1]});S.evaluate(0);begin();S.render();P.commit();
}
function failedEvaluate(){
  S.camera({position:[0,0,0],target:[0,0,0]});rejects(()=>S.evaluate(0));
  begin();rejects(()=>S.render());
}
function recover(){S.camera({position:[0,0,400]});S.evaluate(0);begin();S.render();P.commit();}
function secondScene(){S2.node('leftJoint').place({rotation:[0,0,-.35]});S2.evaluate(3);begin();S2.render();P.commit();}
function callbackGuard(){
  const out={set nodes(value){check(value===5);rejects(()=>S.dispose());throw new Error('observer');}};
  rejects(()=>S.stats(out));S.stats(statsOut); // finally clears both facade/core busy guards.
}
function closeScenes(){
  S.dispose();S.dispose();S2.dispose();D.dispose();
  rejects(()=>HL.place({rotation:[0,0,0]}));rejects(()=>S.render());
  M.stats(modelOut);check(modelOut.instances===0&&modelOut.released&&modelOut.ownedBytes===0);
  // Retained tombstones/handles must not retain the cold blob or native arenas.
}
function forgetScenes(){M=S=S2=D=HL=HR=ROOT=track=motionPlayback=mask=poses=colors=opacity=null;}
