// Same complete cloud and deterministic BODY clock; no opposite-route imports.
'use strict';
(()=>{
  // Private host-fixture initialization, read once before any visual resource.
  const requested=globalThis.__cloudMemoryProfile;
  if(requested!==undefined&&requested!=='DEVICE_SHORT'&&requested!=='HOST_LONG')throw new TypeError('CloudMemory: invalid profile');
  const long=requested==='HOST_LONG',profile=long?'HOST_LONG':'DEVICE_SHORT',expectedDraws=long?1115:63,steady=long?1000:32,
    tag=' profile='+profile+' expectedDraws='+expectedDraws+' steady='+steady,
    V=pocket.kasane,H=V.procedural,resource=H.resource({maxSegments:1024}),
    names=['SELECT','READY','WARM','STEADY','BURST','EXIT','RECREATE1','EXIT1','RECREATE2','EXIT2','COMPLETE'],
    light={direction:[-.5,-.4,.8],color:0xffffff},camera={position:[0,0,400],target:[0,0,0],zoom:Math.fround(.85),center:[120,68],projection:'orthographic'},
    burst={color:0xf5fbff,opacity:.97};
  let variant=null,phase=0,count=0,tick=0,draws=0,run=0,completedRuns=0,held=0,busy=false,
    actor=null,ownedModel=null,retainedScene=null,retainedHandle=null,inputs={body:{},expression:{}},failurePending=false,shown=-1;
  function mark(label){
    console.log('CLOUD_MEM PHASE '+label+' variant='+variant+' run='+run+' draws='+draws+tag);
    if(typeof pocket.app.memoryMark==='function')pocket.app.memoryMark('cloudmem.'+label);
  }
  function release(){
    if(actor){actor.scene.dispose();retainedScene=actor.scene;retainedHandle=actor.model?actor.model.root:null;actor=null;}
    if(ownedModel){ownedModel.dispose();ownedModel=null;}
    inputs={body:{},expression:{}};tick=0;
  }
  function clear(){H.beginFrame(0xe77c);H.commit();}
  function view(tx){
    tx.background(0xe4ece0ff);tx.image({resource,bounds:[0,0,240,135],sourceWidth:240,sourceHeight:135});
    tx.text({bounds:[4,1,236,15],text:'CLOUD MEMORY '+(long?'LONG ':'SHORT ')+(phase===1?variant+' ':'')+names[phase],font:'caption',color:0x294251ff});
    tx.text({bounds:[3,121,237,134],text:variant?'ENTER:RUN U:CLEAR':'LEFT:A RIGHT:C',font:'caption',color:0x294251ff});
  }
  function create(){
    let scene;
    if(variant==='A')scene=Ayatsuri.scene({backend:pocket.hone,geometries:[AyatsuriBlend],light,camera});
    else{ownedModel=pocket.ayatsuri.modelBuiltin('cloud');scene=AyatsuriNative.scene({model:ownedModel,light,camera});}
    // Publish ownership before model binding so failed binding is cleaned next turn.
    actor={scene,model:null};const model=AyatsuriModels.create(scene,0);actor.model=model;
    if(model.sampleFactory)model.sample=model.sampleFactory(model.sample);
  }
  globalThis.AyatsuriCloudMemory={read(out){
    if(busy)throw new Error('CloudMemory: busy');busy=true;
    try{out.variant=variant;out.profile=profile;out.expectedDraws=expectedDraws;out.steadyFrames=steady;
      out.phase=names[phase];out.draws=draws;out.tick=tick;out.run=run;out.completedRuns=completedRuns;
      out.released=!actor&&!ownedModel;out.retained=!!retainedScene&&!!retainedHandle;out.failurePending=failurePending;return out;
    }finally{busy=false;}
  }};
  if(typeof pocket.app.memoryMark!=='function')console.log('CLOUD_MEM METRICS UNAVAILABLE');
  console.log('CLOUD_MEM PROFILE'+tag);
  console.log('CLOUD_MEM RESOURCE bounded procedural maxSegments=1024 retained across runs');
  globalThis.frame=function(buttons){
    if(busy)throw new Error('CloudMemory: busy');busy=true;
    try{
      let press=(buttons|0)&~held;held=buttons|0;
      if(failurePending){release();clear();failurePending=false;phase=variant?1:0;shown=-1;mark('failure');press=0;}
      if(!variant&&(press&0xa0)){variant=press&0x80?'A':'C';phase=1;mark('selected');}
      if(press&0x10){release();clear();phase=variant?1:0;shown=-1;mark('cancel');press=0;}
      if(variant&&(press&0x4000)){
        release();phase=1;shown=-1;draws=count=tick=0;++run;
        console.log('CLOUD_MEM START variant='+variant+' run='+run+' seed=42 breathing=OFF'+tag);mark('start');
        pocket.app.load('follow');pocket.app.load('motion');pocket.app.load('dynamic');
        if(variant==='A'){pocket.app.load('a_utility');pocket.app.load('a_blend');pocket.app.load('a_model');}
        else{pocket.app.load('c_utility');pocket.app.load('c_update');globalThis.AyatsuriModels={create:AyatsuriCloudUpdate.create};}
        mark('loaded');phase=2;
      }
      if(phase>=2&&phase<=9){
        if(phase===5||phase===7||phase===9){
          release();clear();++draws;mark(names[phase]);count=0;
          if(phase===9){phase=10;++completedRuns;mark('complete');console.log('CLOUD_MEM COMPLETE variant='+variant+' run='+run+' draws='+draws+' replayCount='+completedRuns+tag);}
          else ++phase;
        }else{
          if(!actor)create();const ms=tick*1000/30;
          actor.model.sample(ms,0,false,inputs);
          if(phase===4){inputs.body.tilt+=.12*Math.sin(count*.3);inputs.body.stretch=1+.08*Math.sin(count*.2);}
          actor.model.update(ms,inputs.body,inputs.expression);
          if(phase===4){burst.color=count&1?0xeefaff:0xf5fbff;burst.opacity=count&1?.91:.97;actor.model.parts[0].material(burst);}
          actor.scene.evaluate(ms);H.beginFrame(0xe77c);actor.scene.render();H.commit();++draws;++tick;++count;
          if(count===(phase===2?(long?16:4):phase===3?steady:(long?32:8))){mark(names[phase]);++phase;count=0;}
        }
      }else if(shown<0)clear();
      if(shown!==phase){V.replace(view);shown=phase;}
    }catch(error){error=null;failurePending=true;}finally{busy=false;}
  };
})();
