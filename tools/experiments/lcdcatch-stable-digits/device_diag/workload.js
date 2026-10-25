// Disposable diagnostic only. Native key query is unchanged; _diag seeds one press.
const diagFrame=globalThis.frame,diagMark=pocket.input._diag;
let diagTurn=0,diagFrames=0;
globalThis.frame=function(){
  ++diagTurn;
  if(diagTurn<=32){diagFrame(0);return;}
  if(diagTurn===33){diagMark(2);diagFrame(0);return;}
  if(diagTurn<=73){diagFrame(0);return;}
  if(lcdCatch.mode!==2&&lcdCatch.mode!==3)throw Error('LCD diagnostic game path');
  if(diagFrames>=120)throw Error('LCD diagnostic frame limit');
  diagMark(0);diagFrame(0);diagMark(1,lcdCatch.mode,lcdCatch.score);++diagFrames;
};
