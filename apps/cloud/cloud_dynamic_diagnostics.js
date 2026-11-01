// Explicitly loaded diagnostic methods operate on the production adapter state.
'use strict';
globalThis.AyatsuriCloudDynamicDiagnostics={attach(sample,s,reset,read){
  const packet=new Float32Array(2);
  sample.reset=function(){
    if(s.busy)throw new Error('CloudDynamic: busy');s.busy=true;
    try{reset(s);s.t=null;s.l=0;s.external=false;}catch(e){s.z=true;throw e;}finally{s.busy=false;}
  };
  sample.impulse=function(part,value){
    if(s.busy)throw new Error('CloudDynamic: busy');s.busy=true;
    try{
      const i=part==='body'?0:part==='hands'?1:part==='wisp'?2:part==='halo'?3:-1;
      if(i<0||!value||value.length!==2||!Number.isFinite(value[0])||!Number.isFinite(value[1])||
        !Number.isFinite(Math.fround(value[0]))||!Number.isFinite(Math.fround(value[1])))throw new TypeError('CloudDynamic: invalid impulse');
      if(s.t===null||s.z)throw new Error('CloudDynamic: evaluate BODY first');
      packet[0]=value[0];packet[1]=value[1];s.a[i].impulse(packet);read(s,i);s.external=true;
    }finally{s.busy=false;}
  };
  sample.status=function(out){
    if(s.busy)throw new Error('CloudDynamic: busy');s.busy=true;
    try{
      if(!out||typeof out!=='object')throw new TypeError('CloudDynamic: status output required');
      out.simulationStep=s.n;out.timeMs=s.t;out.lastSubsteps=s.l;out.replays=s.r;out.totalSteps=s.w;
      out.seed=s.seed;out.invalid=s.z;out.externalCommands=s.external;out.intent=s.i<0?-1:s.i/9;
      out.bodyResidual=out.handResidual=out.wispResidual=out.bodyVelocity=out.handVelocity=out.wispVelocity=0;
      for(let i=0;i<4;i++)for(let k=0;k<2;k++){
        const r=Math.abs(s.q[i*6+k]-s.q[i*6+4+k]),v=Math.abs(s.q[i*6+2+k]);
        if(i===0){out.bodyResidual=Math.max(out.bodyResidual,r);out.bodyVelocity=Math.max(out.bodyVelocity,v);}
        else if(i===1){out.handResidual=Math.max(out.handResidual,r);out.handVelocity=Math.max(out.handVelocity,v);}
        else{out.wispResidual=Math.max(out.wispResidual,r);out.wispVelocity=Math.max(out.wispVelocity,v);}
      }
      return out;
    }finally{s.busy=false;}
  };
}};
