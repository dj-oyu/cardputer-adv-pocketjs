// Diagnostic copy only. Product frame/render/allocator/scheduler run unchanged.
// State advancement is unmeasured setup, not a full-race FPS claim.
A=null;notes=[];tier=@TIER@;
const diagFrame=globalThis.frame,diagMark=S._diag;
let diagWarm=0,diagFrames=0,diagStage=0;
if('@PROFILE@'!=='pad')enter('gate');
globalThis.frame=function(b){
  // Keep attract idle and reject external inputs in this finite fixed workload.
  idle=0;
  if(diagStage===0){
    diagFrame(0);++diagWarm;
    if(diagWarm>=40&&!queue.length){
      if('@PROFILE@'!=='pad'){enter('race');diagStage=1;}
      else diagStage=2;
    }
    return;
  }
  if(diagStage===1){
    // Return to the product owner between bounded batches. A synchronous
    // advance to 390m exceeded the device's normal per-turn watchdog.
    for(let n=0;n<8&&mx.apply(null,rs.x)<390;++n){
      step(F,rs,DT);
      for(let i=0;i<8;++i)ph[i]+=rs.v[i]*DT/6.5*2*PI;
      at(1);
    }
    if(mx.apply(null,rs.x)>=390){
      drop(['gate']);want(['band']);wide=()=>8;diagStage=2;
    }
    return;
  }
  // Ten frames finish the scene transition and band registration before timing.
  if(diagFrames++<10){diagFrame(0);return;}
  if('@PROFILE@'!=='pad'){cm=8;hold=9999;dl=0;man=0;}
  diagMark(0);
  diagFrame(0);
  diagMark(1,rs?mx.apply(null,rs.x):0);
};
