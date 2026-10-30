// Manual visual comparison, not a claim of completed character acting.
'use strict';
(()=>{
  const K=pocket.kasane,H=K.procedural,R=H.resource({maxSegments:1024}),
    direction=[-.5,-.4,.8],light={direction,color:0xffffff},
    camera={position:[0,0,400],target:[0,0,0],zoom:1.3,center:[120,78],projection:'orthographic'},
    pose={yaw:0,pitch:0,smile:0,surprise:0,jawOpen:0},names=['NEUTRAL','SMILE','SURPRISE','JAW OPEN'];
  let scene=null,asset=null,model=null,expression=0,yaw=0,pitch=0,lightYaw=0,held=0,tick=0,busy=false,shown=-1;
  // Construct after the entry/load turn retires its VM frames, as other labs do.
  // Publish all three owners together; a failed creation leaves a retryable app.
  function createActor(){
    if(model)return;
    let nextScene=null,nextAsset=null;
    try{nextAsset=pocket.ayatsuri.modelBuiltin('fox-face');nextScene=AyatsuriClip.nativeScene({model:nextAsset,light,camera});
      const nextModel=FoxFaceModel.create(nextScene);scene=nextScene;asset=nextAsset;model=nextModel;
    }catch(error){if(nextScene)nextScene.dispose();if(nextAsset)nextAsset.dispose();throw error;}
  }
  function finite(v,lo,hi){
    if(typeof v!=='number'||!Number.isFinite(v)||v<lo||v>hi)throw new RangeError('FoxFaceLab: view range');return v;
  }
  function select(e,y,p,l){
    if(busy)throw new Error('FoxFaceLab: active');
    finite(e,0,3);if(e%1)throw new RangeError('FoxFaceLab: expression');
    finite(y,-35,35);finite(p,-10,10);finite(l,-180,180);
    expression=e;yaw=y;pitch=p;lightYaw=l;
  }
  // App-owned deterministic comparison input, unrelated to the framework clock.
  globalThis.FoxFaceLab={select,read(out){
    if(busy)throw new Error('FoxFaceLab: active');
    if(!out||typeof out!=='object')throw new TypeError('FoxFaceLab: output');
    busy=true;try{out.expression=expression;out.yaw=yaw;out.pitch=pitch;out.lightYaw=lightYaw;out.tick=tick;}finally{busy=false;}
    return out;
  }};
  function view(tx){
    tx.background(0xe4ece0ff);tx.image({resource:R,bounds:[0,0,240,135],sourceWidth:240,sourceHeight:135});
    tx.text({bounds:[3,0,237,14],text:'FOX FACE '+names[expression],font:'caption',color:0x294251ff});
    tx.text({bounds:[2,121,238,134],text:'L/R:YAW U:FACE D:LIGHT ENTER:RESET',font:'caption',color:0x294251ff});
  }
  globalThis.frame=function(buttons){
    if(busy)throw new Error('FoxFaceLab: active');busy=true;
    try{
      createActor();
      const press=(buttons|0)&~held;held=buttons|0;
      if(press&0x4000){expression=0;yaw=0;pitch=0;lightYaw=0;tick=0;}
      if(press&0x10)expression=(expression+1)%4;
      if(press&0x80)yaw=Math.max(-35,yaw-35);
      if(press&0x20)yaw=Math.min(35,yaw+35);
      if(press&0x40)lightYaw=lightYaw===0?60:lightYaw===60?-60:0;
      const angle=lightYaw*Math.PI/180,c=Math.cos(angle),s=Math.sin(angle);
      direction[0]=-.5*c+.8*s;direction[1]=-.4;direction[2]=.5*s+.8*c;scene.light(light);
      pose.yaw=yaw*Math.PI/180;pose.pitch=pitch*Math.PI/180;
      pose.smile=expression===1?1:0;pose.surprise=expression===2?1:0;pose.jawOpen=expression===3?1:0;
      model.update(pose);scene.evaluate(tick*1000/30);
      H.beginFrame(0xe77c);scene.render();H.commit();++tick;
      if(shown!==expression){K.replace(view);shown=expression;}
    }finally{busy=false;}
  };
})();
