// Cloud-only analytic choreography. No mutable physics or shared clock.
'use strict';
globalThis.AyatsuriCloudMotion = (() => {
  function ease(t,a,b){const u=Math.max(0,Math.min(1,(t-a)/(b-a)));return u*u*(3-2*u);}
  function phrase(t,a,b,c,d){return ease(t,a,b)*(1-ease(t,c,d));}
  function settle(t){
    if(t<=6.5||t>=7.65)return 0;
    const u=t-6.5;
    return Math.sin(u*14)*Math.exp(-u*3)*(1-ease(t,6.5,7.65));
  }
  function wisp(t){
    const u=t-4.28,recoil=u>0&&u<.67?
      Math.sin(u*14)*Math.exp(-u*5)*(1-ease(t,4.28,4.95)):0;
    return phrase(t,2.35,3.05,3.48,4.28)-.28*phrase(t,5,5.28,5.4,5.66)+
      .4*phrase(t,5.45,5.9,6.05,6.5)+.6*recoil+.6*settle(t);
  }
  function spread(t){
    return Math.max(.65*phrase(t,2.28,2.6,3.05,3.8),.8*phrase(t,5.65,6.02,6.25,6.85));
  }
  return function(timeMs,mode,breathing,inputs){
    const body=inputs.body,expression=inputs.expression;
    body.shiftX=body.handLeft=body.handRight=body.haloFollow=0;
    body.wispCenter=body.wispLeft=body.wispRight=0;
    body.wispSpreadCenter=body.wispSpreadLeft=body.wispSpreadRight=0;
    expression.gazeX=expression.gazeY=expression.smile=expression.brow=0;
    expression.surprise=0;
    if(mode===0){
      body.tilt=body.turn=body.lift=body.head=body.tail=body.haloOffset=0;
      body.stretch=1;expression.open=1;expression.blink=0;
      const t=Math.max(0,timeMs)/1000;
      // Look first, gather weight, lean, then recover with a buoyant release.
      // Every phrase has a compact support; seek order cannot leave a spring running.
      const anticipate=phrase(t,2,2.22,2.35,2.66),
        lean=phrase(t,2.35,3.05,3.48,4.28),
        compress=phrase(t,5,5.28,5.4,5.66),
        extend=phrase(t,5.45,5.9,6.05,6.5),s=settle(t),
        delayed=phrase(t-.16,2.35,3.05,3.48,4.28),
        look=phrase(t,2,2.16,3.6,4.18);
      body.tilt=-.065*anticipate+.27*lean+.035*s;
      body.turn=.18*lean-.025*anticipate;
      body.shiftX=-1.2*anticipate+5*lean;
      body.head=.1*look-.04*anticipate-.055*delayed;
      body.stretch=1-.045*anticipate-.16*compress+.19*extend+.025*s;
      body.lift=1.8*anticipate-1.4*lean+3.5*compress-5*extend+1.1*s;
      body.handLeft=.12*anticipate+.22*delayed+.10*extend-.05*settle(t-.09);
      body.handRight=-.07*anticipate-.15*phrase(t-.26,2.35,3.05,3.48,4.28)+.16*extend;
      body.haloOffset=-4*delayed+1.2*settle(t-.16);
      body.haloFollow=-.12*delayed+.035*settle(t-.16);
      // Local-Y pixel offsets: the tips bend after the center, not as one plate.
      body.wispCenter=-.45*wisp(t)||0;
      body.wispLeft=1.6*wisp(t-.08);
      body.wispRight=-2.2*wisp(t-.16)||0;
      // Dimensionless expansion, distinct from the local-Y bending above.
      body.wispSpreadCenter=spread(t);
      body.wispSpreadLeft=spread(t-.1);
      body.wispSpreadRight=spread(t-.2);
      expression.gazeX=.68*look;
      expression.gazeY=-.18*phrase(t,5.2,5.6,6.1,6.65);
      expression.surprise=phrase(t,2.16,2.3,2.47,2.78);
      expression.smile=.9*phrase(t,2.9,3.22,3.65,4.14)+.8*phrase(t,5.6,5.95,6.25,6.75);
      expression.brow=.32*look-.16*compress;
      expression.open=1-.12*anticipate+.035*extend;
      expression.blink=.8*phrase(t,4.35,4.43,4.48,4.6);
      if(breathing)body.lift+=Math.sin(t*2)*1.2;
    }
    return inputs;
  };
})();
