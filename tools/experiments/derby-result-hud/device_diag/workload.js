// Diagnostic copy only: real result frame, allocator, scheduler and LCD path.
A=null;notes=[];tier=1;
const diagFrame=globalThis.frame,diagMark=S._diag;
let diagWarm=0,diagFrames=0,diagReady=false;
enter('gate');
globalThis.frame=function(b){
  idle=0;
  if(!diagReady){
    diagFrame(0);++diagWarm;
    if(diagWarm>=40&&!queue.length){
      enter('race');let steps=0;
      while(rs.done<3&&steps++<6000){
        step(F,rs,DT);
        for(let i=0;i<8;++i)ph[i]+=rs.v[i]*DT/6.5*2*PI;
        at(1);
      }
      if(rs.done<3)throw Error('result diagnostic model did not finish');
      settle();enter('res');diagReady=true;
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
