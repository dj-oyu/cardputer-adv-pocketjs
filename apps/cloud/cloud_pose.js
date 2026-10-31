// Pure facial/shape targets only. Dynamic roll, hands and wisps own their q/v.
'use strict';
globalThis.AyatsuriCloudMotion=(()=>{
  function ease(t,a,b){const u=Math.max(0,Math.min(1,(t-a)/(b-a)));return u*u*(3-2*u);}
  function phrase(t,a,b,c,d){return ease(t,a,b)*(1-ease(t,c,d));}
  function settle(t){if(t<=6.5||t>=7.65)return 0;const u=t-6.5;return Math.sin(u*14)*Math.exp(-u*3)*(1-ease(t,6.5,7.65));}
  function spread(t){return Math.max(.65*phrase(t,2.28,2.6,3.05,3.8),.8*phrase(t,5.65,6.02,6.25,6.85));}
  return function(timeMs,mode,breathing,inputs){
    const body=inputs.body,expression=inputs.expression;
    body.shiftX=body.handLeft=body.handRight=body.haloFollow=0;
    body.wispCenter=body.wispLeft=body.wispRight=0;
    body.wispSpreadCenter=body.wispSpreadLeft=body.wispSpreadRight=0;
    expression.gazeX=expression.gazeY=expression.smile=expression.brow=expression.surprise=0;
    if(mode===0){
      body.tilt=body.turn=body.lift=body.head=body.tail=body.haloOffset=0;
      body.stretch=1;expression.open=1;expression.blink=0;
      const t=Math.max(0,timeMs)/1000,a=phrase(t,2,2.22,2.35,2.66),l=phrase(t,2.35,3.05,3.48,4.28),
        c=phrase(t,5,5.28,5.4,5.66),e=phrase(t,5.45,5.9,6.05,6.5),s=settle(t),
        d=phrase(t-.16,2.35,3.05,3.48,4.28),g=phrase(t,2,2.16,3.6,4.18);
      body.turn=.18*l-.025*a;body.shiftX=-1.2*a+5*l;
      body.stretch=1-.045*a-.16*c+.19*e+.025*s;
      body.lift=1.8*a-1.4*l+3.5*c-5*e+1.1*s;
      body.haloFollow=-.12*d+.035*settle(t-.16);
      body.wispSpreadCenter=spread(t);body.wispSpreadLeft=spread(t-.1);body.wispSpreadRight=spread(t-.2);
      expression.gazeX=.68*g;expression.gazeY=-.18*phrase(t,5.2,5.6,6.1,6.65);
      expression.surprise=phrase(t,2.16,2.3,2.47,2.78);
      expression.smile=.9*phrase(t,2.9,3.22,3.65,4.14)+.8*phrase(t,5.6,5.95,6.25,6.75);
      expression.brow=.32*g-.16*c;expression.open=1-.12*a+.035*e;
      expression.blink=.8*phrase(t,4.35,4.43,4.48,4.6);
      if(breathing)body.lift+=Math.sin(t*2)*1.2;
    }
    return inputs;
  };
})();
