// Diagnostic copy only: naturally bounded photo frames, no scene extension.
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
        const was=rs.done;step(F,rs,DT);
        for(let i=0;i<8;++i)ph[i]+=rs.v[i]*DT/6.5*2*PI;
        if(!was&&rs.done){const w=order(rs)[0];photoX=photo((rs.tc[w]-rs.t+DT)/DT);}
        at(1);
      }
      if(rs.done<3||!photoX)throw Error('photo diagnostic model did not finish');
      settle();enter('photo');diagReady=true;
    }
    return;
  }
  if(diagFrames++<10){diagFrame(0);return;}
  if(dm!==0||scene!=='photo'||need||queue.length||t!==diagFrames-1)
    throw Error('photo diagnostic requires natural settled photo frames');
  diagMark(0);
  diagFrame(0);
  diagMark(1,mx.apply(null,rs.x));
};
