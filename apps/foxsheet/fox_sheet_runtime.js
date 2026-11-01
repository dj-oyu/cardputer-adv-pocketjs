// Neutral native surface viewer. Input owns view targets; no hidden animation clock.
'use strict';
(()=>{
  const K=pocket.kasane,actor=FoxSheetScene.create(globalThis.FoxSheetData);
  globalThis.FoxSheetData=null;
  let yaw=0,lightYaw=0,loaded=true,held=0,tick=0,busy=false,shown=-1;
  function finite(v,limit){if(typeof v!=='number'||!Number.isFinite(v)||Math.abs(v)>limit)throw RangeError('FoxSheet: view range');return v;}
  function alive(){if(busy)throw Error('FoxSheet: active');}
  function select(y,l){alive();finite(y,35);finite(l,180);yaw=y;lightYaw=l;loaded=true;}
  function release(){alive();busy=true;try{actor.release();loaded=false;}finally{busy=false;}}
  function read(out){alive();if(!out||typeof out!=='object')throw TypeError('FoxSheet: output');
    busy=true;try{out.loaded=loaded;out.yaw=yaw;out.lightYaw=lightYaw;out.tick=tick;}finally{busy=false;}return out;
  }
  globalThis.FoxSheetApp={select,release,read};
  function view(tx){
    actor.view(tx);
    tx.text({bounds:[3,0,237,14],text:loaded?'FOX SHEET NEUTRAL':'FOX SHEET RELEASED',font:'caption',color:0x294251ff});
    tx.text({bounds:[2,121,238,134],text:'LR:YAW D:LIGHT U:LOAD/FREE ENT:RESET',font:'caption',color:0x294251ff});
  }
  globalThis.frame=function(buttons){
    alive();busy=true;
    try{
      const press=(buttons|0)&~held;held=buttons|0;
      if(press&0x4000){yaw=lightYaw=0;loaded=true;tick=0;}
      if(press&0x80)yaw=Math.max(-35,yaw-35);
      if(press&0x20)yaw=Math.min(35,yaw+35);
      if(press&0x40)lightYaw=lightYaw===0?60:lightYaw===60?-60:0;
      if(press&0x10){if(loaded){actor.release();loaded=false;}else loaded=true;}
      if(loaded)actor.draw(yaw,lightYaw);
      // Releasing a Model does not invalidate the already submitted numeric
      // frame. The app-owned display resource intentionally keeps that image.
      // The view labels depend only on lifecycle, not pose or light targets.
      const key=loaded?1:0;
      if(shown!==key){K.replace(view);shown=key;}
      ++tick;
    }finally{busy=false;}
  };
})();
