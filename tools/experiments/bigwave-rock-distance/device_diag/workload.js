// Disposable diagnostic: the existing host bot's read-only decisions, outside the JS timer.
const diagFrame=globalThis.frame,diagMark=pocket.input._diag;
let diagTurn=0,diagFrames=0;
function diagKeys(){
 const S=bigWave.S,c=S.course;let lo=.34,hi=.5,tuck=0,pop=0;
 for(const k of c.secs)if(k.x+k.w>S.x-2&&k.x<S.x+12&&bigWave.curlAt(k.x+k.w/2)>.5){lo=.08;hi=.3;tuck=1;}
 const dodge=(p,g,w0,w1)=>{if(g>w0&&g<w1&&Math.abs(p-S.p)<.2)pop=1;
  if(!tuck&&g>0&&g<14&&Math.abs(p-(lo+hi)/2)<.22){lo=p>.45?Math.max(.05,p-.4):Math.min(.8,p+.28);hi=lo+.12;}};
 for(const k of c.rocks)dodge(k.p,k.x-S.x,2.2,3.4);
 for(const k of c.riders){const t=k.t0===undefined?0:(S.t-k.t0)/30;
  dodge(Math.min(.9,Math.max(.05,k.p+.15*Math.sin(.9*t+k.ph))),k.x+k.v*t-S.x,1.8,3);}
 const q=S.x-S.xc;let mask=pop&&S.z<=0?32:0;
 if(tuck||q<8.5)mask|=16;
 if(q<12.5)mask|=1;else if(q>17)mask|=2;
 let want=3*((lo+hi)/2-S.p);want=Math.min(.5,Math.max(-.5,want));if(S.p>=lo&&S.p<=hi)want=0;
 if(S.h<want-.08)mask|=4;else if(S.h>want+.08)mask|=8;
 return mask;
}
globalThis.frame=function(){
 ++diagTurn;
 diagMark(2,diagTurn===41?64:diagTurn>42?diagKeys():0);
 if(diagTurn<=82){diagFrame(0);return;}
 if(bigWave.S.st!==1||bigWave.S.pause||bigWave.S.tier!==1||bigWave.queue.length)throw Error('rock diagnostic path');
 if(diagFrames>=120)throw Error('rock diagnostic frame limit');
 diagMark(0);diagFrame(0);diagMark(1,bigWave.S.st,bigWave.S.x);++diagFrames;
};
