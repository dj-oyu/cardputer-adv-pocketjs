// Diagnostic copy only: real result frame, allocator, scheduler and LCD path.
A=null;notes=[];tier=1;
const diagFrame=globalThis.frame,diagMark=S._diag;
let diagWarm=0,diagFrames=0,diagReady=false,diagAdvancing=false,diagSteps=0;
enter('gate');
globalThis.frame=function(b){
  idle=0;
  if(!diagReady){
    if(!diagAdvancing){
      diagFrame(0);++diagWarm;
      if(diagWarm<40||queue.length)return;
      enter('race');diagAdvancing=true;
    }
    // Split both the race and settlement across owner turns, without new budgets.
    for(let batch=0;batch<8&&!diagReady;++batch){
      if(rs.done>=8||rs.done>=3&&rs.t>=200){

        settle();enter('res');diagReady=true;break;
      }
      if(++diagSteps>6000)throw Error('res diagnostic setup step limit');
      if(rs.done<3){
        step(F,rs,DT);
        for(let i=0;i<8;++i)ph[i]+=rs.v[i]*DT/6.5*2*PI;
        at(1);
      }else step(F,rs,DT); // settle's remaining field, with the same update order.
    }
    return;
  }
  if(diagFrames++<10){diagFrame(0);return;}
  if(dm!==0||scene!=='res'||need||queue.length||R.t[3].s===undefined)
    throw Error('result diagnostic requires settled result labels');
  diagMark(0);
  diagFrame(0);
  diagMark(1,mx.apply(null,rs.x));
};
