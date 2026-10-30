// Full cloud model, fixed world light/camera, and a finite optional fixture.
'use strict';
(()=>{
  const V=pocket.kasane,H=V.procedural,resource=H.resource({maxSegments:1024}),
    light={direction:[-.5,-.4,.8],color:0xffffff},
    camera={position:[0,0,400],target:[0,0,0],zoom:.85,center:[120,68],projection:'orthographic'},
    inputs={body:{},expression:{}};
  let actor=null,tick=0,held=0,busy=false,frameActive=false,diagnostics=null,shown=-1;
  function discard(){if(actor)actor.scene.dispose();actor=null;inputs.body={};inputs.expression={};tick=0;shown=-1;}
  function idle(){if(busy)throw new Error('CloudProbe: frame active');}
  const probe=globalThis.AyatsuriCloudProbe={failurePending:false,attachControl(factory){
    idle();if(diagnostics)throw new Error('CloudProbe: already attached');
    diagnostics=factory(function(op){
      if(op==='lock'){idle();busy=true;}else if(op==='unlock')busy=false;
      else if(op==='discard')discard();
    });return diagnostics.control;
  }};
  function view(tx){
    tx.background(0xe4ece0ff);tx.image({resource,bounds:[0,0,240,135],sourceWidth:240,sourceHeight:135});
    tx.text({bounds:[4,1,236,15],text:diagnostics&&diagnostics.control.active?'CLOUD FIXTURE':'CLOUD NORMAL',font:'caption',color:0x294251ff});
    tx.text({bounds:[3,121,237,134],text:'ENTER:FIXTURE U:RESET',font:'caption',color:0x294251ff});
  }
  globalThis.frame=function(buttons){
    if(frameActive)throw new Error('CloudProbe: frame active');
    idle();frameActive=true;
    try{
    if(probe.failurePending){probe.failurePending=false;probe.failure();}
    let press=(buttons|0)&~held;held=buttons|0;
    if(press&0x4000){
      discard();try{pocket.app.load('fixture');probe.enter();}catch(error){console.log('CLOUD_FIXTURE RETURN reason=failure');}
      press&=~0x40f0;
    }
    busy=true;
    try{
      if(press&0x10){tick=0;if(diagnostics)diagnostics.reset();}
      if(!actor){const scene=Ayatsuri.scene({backend:pocket.hone,geometries:[AyatsuriBlend],light,camera}),model=AyatsuriModels.create(scene,0);
        if(model.sampleFactory)model.sample=model.sampleFactory(model.sample);actor={scene,model};}
      const t=diagnostics?diagnostics.sample(tick):tick,ms=t*1000/30;
      actor.model.sample(ms,0,false,inputs);actor.model.update(ms,inputs.body,inputs.expression);actor.scene.evaluate(ms);
      H.beginFrame(0xe77c);actor.scene.render();H.commit();
      tick=diagnostics?diagnostics.submitted(t):t+1;
      const active=diagnostics&&diagnostics.control.active?1:0;
      if(shown!==active){V.replace(view);shown=active;}
    }catch(error){
      busy=false;
      if(!diagnostics||!diagnostics.control.active||!probe.failure)throw error;
      // Cleanup runs next turn, after the caught exception and VM frame retire.
      error=null;probe.failurePending=true;
    }finally{busy=false;}
    }finally{frameActive=false;}
  };
})();
