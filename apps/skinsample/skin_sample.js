'use strict';
// Explicit caller-owned clock. No timer, global renderer reset or retained rig inputs.
globalThis.SkinSample={start(blob){
  const native=pocket.ayatsuri,P=pocket.kasane.shiki;
  const G=new Float32Array(24),IB=new Float32Array(24),V=new Float32Array(36);
  const J=new Uint16Array(24),W=new Uint16Array(24);
  const pose=new Float32Array([15,0,0,0,0,0,1,1,1]),child=G.subarray(12);
  for(let j=0;j<24;j+=12){G[j]=G[j+5]=G[j+10]=1;IB[j]=IB[j+5]=IB[j+10]=1;}
  IB[15]=-15;
  for(let v=0;v<6;v++){
    V[v*6]=(v%3)*30-30;V[v*6+1]=v<3?-15:15;V[v*6+5]=1;J[v*4+1]=1;
    W[v*4+1]=v%3===0?0:v%3===1?21845:65535;W[v*4]=65535-W[v*4+1];
  }
  const playback=AyatsuriMotion.performance({
    track:AyatsuriMotion.track(new Float32Array([0,0,0,0,11900,0,0,0]),{stride:4,timeUnit:'ms'}),
    channels:['time','y','z','angle'],waves:[
      {channel:'y',amplitude:8,rate:.0007,mode:'add'},
      {channel:'z',amplitude:12,rate:.0013,mode:'add'},
      {channel:'angle',amplitude:.7,rate:.0011,mode:'add'}]});
  let model=null,scene=null,stopped=false;
  try{model=native.modelOpen(blob);scene=AyatsuriNative.scene({model,
    camera:{position:[0,0,400],target:[0,0,0],zoom:1,center:[120,67]},
    light:{direction:[0,0,1],color:0xffffff}});
    return {frame(timeMs){
      if(stopped)throw Error('skin sample stopped');
      if(typeof timeMs!=='number'||!Number.isFinite(timeMs))throw Error('finite ms required');
      const value=playback.evaluate(timeMs);
      pose[1]=value.y;pose[2]=value.z;pose[4]=value.angle;
      native.compose(pose,null,child);
      scene.evaluateSkin(timeMs,1,G,IB,V,J,W);
      P.beginMeshFrame(17,2048,200000,1);scene.render();P.commit();
    },stop(){if(stopped)return;scene.dispose();model.dispose();stopped=true;scene=model=null;}};
  }catch(error){if(scene)scene.dispose();if(model)model.dispose();throw error;}
}};
