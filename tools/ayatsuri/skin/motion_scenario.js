'use strict';
// Test-only app adapter. No Scene internals, wire format, importer or timers.
// Existing test_js drives frame IDs; convert once to explicit milliseconds.
const motionTrack=AyatsuriMotion.track(new Float32Array([
  0,0,0,0, 11900,0,0,0
]),{stride:4,timeUnit:'ms'});
const motionPlayback=AyatsuriMotion.performance({track:motionTrack,
  channels:['time','y','z','angle'],waves:[
    {channel:'y',amplitude:8,rate:.0007,mode:'add'},
    {channel:'z',amplitude:12,rate:.0013,mode:'add'},
    {channel:'angle',amplitude:.7,rate:.0011,mode:'add'}]});
let borrowedMotion=null;
poseAt=function(frame){
  const timeMs=frame*100, value=motionPlayback.evaluate(timeMs);
  check(motionPlayback.timeUnit==='ms');
  if(borrowedMotion)check(value===borrowedMotion);
  borrowedMotion=value;
  // Independent analytic channel expectations, not another Motion invocation.
  check(Math.abs(value.y-8*Math.sin(frame*.07))<1e-10);
  check(Math.abs(value.z-12*Math.sin(frame*.13))<1e-10);
  check(Math.abs(value.angle-.7*Math.sin(frame*.11))<1e-10);
  pose[1]=value.y;pose[2]=value.z;pose[4]=value.angle;
  pocket.ayatsuri.compose(pose,null,child); // mesh-local globals, bind offset in IB
};
function invalidMotion(){
  poseAt(30);const saved=G.slice(), previous=borrowedMotion.y;
  rejects(()=>poseAt(NaN));rejects(()=>poseAt(Infinity));
  check(borrowedMotion.y===previous);
  for(let i=0;i<G.length;i++)check(G[i]===saved[i]);
}
