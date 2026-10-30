// Optional app diagnostics. Load explicitly with pocket.app.load('diagnostics').
// Running tick is the next sample cursor. Paused step submits tick+1.
// lastSubmitted identifies H.commit success, never display acknowledgement.
'use strict';
AyatsuriLab.control=AyatsuriLab.attachControl(function(access){
  let paused=false,pendingSteps=0,lastSubmittedTick=-1,lastSubmittedModel=-1,
    lastSubmittedMode=-1,lastSubmittedBreathing=false;
  const state={};
  function integer(value,max){return Number.isSafeInteger(value)&&value>=0&&value<=max;}
  function set(values){
    access('lock');
    try{
      if(!values||typeof values!=='object')throw new Error('AyatsuriLab.control: expected settings');
      access('read',state);
      let nextPaused=paused;
      for(const key of Object.keys(values)){
        const value=values[key];
        if((key==='model'&&integer(value,2))||(key==='mode'&&integer(value,5))||
            (key==='tick'&&integer(value,Number.MAX_SAFE_INTEGER))||
            (key==='breathing'&&typeof value==='boolean'))state[key]=value;
        else if(key==='paused'&&typeof value==='boolean')nextPaused=value;
        else throw new Error('AyatsuriLab.control: invalid '+key);
      }
      access('write',state);paused=nextPaused;pendingSteps=0;
    }finally{access('unlock');}
  }
  return {control:{set,seek(value){set({tick:value});},pause(value=true){set({paused:value});},
    step(count=1){
      access('lock');
      try{
        access('read',state);
        if(!paused||!integer(count,600)||!count||pendingSteps+count>600||
            state.tick>Number.MAX_SAFE_INTEGER-pendingSteps-count)
          throw new Error('AyatsuriLab.control: step requires paused and 1..600 available ticks');
        pendingSteps+=count;
      }finally{access('unlock');}
    },read(out){
      if(!out||typeof out!=='object')throw new Error('AyatsuriLab.control: expected output object');
      access('read',out);out.paused=paused;out.pendingSteps=pendingSteps;
      out.lastSubmittedTick=lastSubmittedTick;out.lastSubmittedModel=lastSubmittedModel;
      out.lastSubmittedMode=lastSubmittedMode;out.lastSubmittedBreathing=lastSubmittedBreathing;
      return out;
    }},
    reset(){pendingSteps=0;},sample(tick){return tick+(paused&&pendingSteps?1:0);},
    submitted(tick,model,mode,breathing){
      lastSubmittedTick=tick;lastSubmittedModel=model;lastSubmittedMode=mode;
      lastSubmittedBreathing=breathing;
      if(paused){if(pendingSteps)--pendingSteps;return tick;}
      if(tick<Number.MAX_SAFE_INTEGER)return tick+1;
      paused=true;return tick;
    }};
});
