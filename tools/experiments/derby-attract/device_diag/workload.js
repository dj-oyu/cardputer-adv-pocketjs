// Diagnostic copy only. Product frame/render/allocator/scheduler run unchanged.
// State advancement is unmeasured setup, not a full-race FPS claim.
A=null;notes=[];tier=@TIER@;
const diagFrame=globalThis.frame,diagMark=S._diag;
diagMark(2,@OPTIMIZED@); // Startup validation only; identical native C in A/B.
let diagWarm=0,diagFrames=0,diagReady=false,diagAdvancing=false,diagSteps=0;
if('@PROFILE@'!=='pad')enter('gate');
globalThis.frame=function(b){
  // Keep attract idle and reject external inputs in this finite fixed workload.
  idle=0;
  if(!diagReady){
    if(!diagAdvancing){
      diagFrame(0);++diagWarm;
      if(diagWarm<40||queue.length)return;
      if('@PROFILE@'==='pad'){diagReady=true;return;}
      enter('race');diagAdvancing=true;
    }
    // Return to the owner after at most eight model steps; retain its watchdog.
    for(let batch=0;batch<8&&mx.apply(null,rs.x)<390;++batch){
      if(++diagSteps>6000)throw Error('attract diagnostic setup step limit');
      step(F,rs,DT);
      for(let i=0;i<8;++i)ph[i]+=rs.v[i]*DT/6.5*2*PI;
      at(1);
    }
    if(mx.apply(null,rs.x)>=390){
      drop(['gate']);want(['band']);wide=()=>8;diagReady=true;
    }
    return;
  }
  // Ten frames finish the scene transition and band registration before timing.
  if(diagFrames++<10){diagFrame(0);return;}
  if('@PROFILE@'!=='pad'){cm=8;hold=9999;dl=0;man=0;}
  if(dm!==0||scene!=='race')throw Error('attract diagnostic must be manual race');
  diagMark(0);
  diagFrame(0);
  diagMark(1,rs?mx.apply(null,rs.x):0);
};
