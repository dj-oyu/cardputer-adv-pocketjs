// Diagnostic copy only: naturally bounded photo frames, no scene extension.
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
        if(!photoX)throw Error('photo diagnostic snapshot missing');
        settle();enter('photo');diagReady=true;break;
      }
      if(++diagSteps>6000)throw Error('photo diagnostic setup step limit');
      if(rs.done<3){
        const was=rs.done;step(F,rs,DT);
        for(let i=0;i<8;++i)ph[i]+=rs.v[i]*DT/6.5*2*PI;
        if(!was&&rs.done){const w=order(rs)[0];photoX=photo((rs.tc[w]-rs.t+DT)/DT);}
        at(1);
      }else step(F,rs,DT); // settle's remaining field, with the same update order.
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
