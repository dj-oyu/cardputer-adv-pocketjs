// A deterministic 10-second body test separates motion from light and view.
'use strict';
globalThis.AyatsuriLab = (() => {
  const modes=['BODY','LIGHT','CAMERA','TEXTURE','EXPRESSION','STILL'];
  const body={tilt:0,turn:0,stretch:1,lift:0,head:0,tail:0,haloOffset:0},
    expression={open:1,blink:0}, light={direction:[-.5,-.4,.8],color:0xffffff},
    camera={position:[0,0,400],target:[0,0,0],zoom:.85,center:[120,68],projection:'orthographic'},
    texture={offset:[0,0],scale:[1,1],rotation:0};
  const inputs={body,expression,light,camera,texture};
  function resetInputs(){
    body.tilt=body.turn=body.lift=body.head=body.tail=body.haloOffset=0;
    body.stretch=1;expression.open=1;expression.blink=0;
    light.direction[0]=-.5;light.direction[1]=-.4;light.direction[2]=.8;
    camera.position[0]=0;camera.position[1]=0;camera.position[2]=400;
    texture.offset[0]=texture.offset[1]=texture.rotation=0;
  }
  function sample(timeMs,mode,breathing){
    resetInputs();
    const t=Math.max(0,timeMs)/1000;
    if(mode===0){
      if(t>=2&&t<5)body.tilt=.3*Math.sin((t-2)*Math.PI*2/3);
      if(t>=5&&t<8)body.stretch=1+.18*Math.sin((t-5)*Math.PI*2/3);
      // The halo's offset is an explicit analytic delay, not a mutable spring.
      const d=t-.18;
      if(d>=2&&d<5)body.haloOffset=-3*Math.sin((d-2)*Math.PI*2/3);
      body.tail=body.tilt*.6;
      if(breathing)body.lift=Math.sin(t*2)*1.2;
    } else if(mode===1)light.direction[0]=.8*Math.sin(t*.8);
    else if(mode===2)camera.position[0]=100*Math.sin(t*.7);
    else if(mode===3)texture.offset[1]=.12*Math.sin(t*.8);
    else if(mode===4){
      expression.open=.6+.25*Math.sin(t*.5);
      const phase=t%3;
      if(phase>=1&&phase<1.3)expression.blink=Math.sin((phase-1)/.3*Math.PI);
    }
    return inputs;
  }
  function create(index,backend){
    resetInputs();
    const scene=Ayatsuri.scene({backend,light,camera,
      geometries:AyatsuriModels.needsBlend?[AyatsuriBlend]:undefined});
    const model=AyatsuriModels.create(scene,index);
    // Finish visual construction before allocating persistent motion state.
    // Model-building scratch and closures need not overlap actor setup.
    if(model.sampleFactory)model.sample=model.sampleFactory(model.sample);
    function evaluate(timeMs,mode,breathing){
      sample(timeMs,mode,breathing);
      if(model.sample)model.sample(timeMs,mode,breathing,inputs);
      model.update(timeMs,body,expression);
      scene.light(light);scene.camera(camera);
      for(let i=0;i<model.patterns.length;i++)model.patterns[i].texture(texture);
      scene.evaluate(timeMs);
    }
    return {scene,model,evaluate};
  }
  return {modes,sample,create};
})();

if(typeof pocket!=='undefined'&&pocket.kasane){
  const V=pocket.kasane,H=V.procedural;
  const resource=H.resource({maxSegments:1024});
  let actor=null,actorIndex=-1,index=0,mode=0,tick=0,held=0,breathing=false,rebuild=true;
  let busy=false,diagnostics=null;
  function idle(){if(busy)throw new Error('AyatsuriLab.control: frame is active');}
  // Only the explicitly loaded diagnostics chunk constructs these hooks/state.
  AyatsuriLab.attachControl=function(factory){
    idle();
    if(diagnostics)throw new Error('AyatsuriLab.control: already attached');
    diagnostics=factory(function(operation,value){
      if(operation==='lock'){idle();busy=true;}
      else if(operation==='unlock')busy=false;
      else if(operation==='write'){
        index=value.model;mode=value.mode;tick=value.tick;breathing=value.breathing;
      }else{
        value.model=index;value.mode=mode;value.tick=tick;value.breathing=breathing;
      }
    });
    return diagnostics.control;
  };
  const stats={};
  let title,shown=-1;
  function label(){return AyatsuriModels.names[index]+'  '+AyatsuriLab.modes[mode]+(breathing?'  BREATHE':'');}
  function build(tx){
    tx.background(0xe4ece0ff);
    tx.image({resource,bounds:[0,0,240,135],sourceWidth:240,sourceHeight:135});
    title=tx.text({bounds:[4,1,236,15],text:label(),capacity:40,
      font:'caption',color:0x294251ff});
    shown=index*12+mode*2+(breathing?1:0);
    tx.text({bounds:[3,121,237,134],text:'ENTER:NEXT L/R:TEST U:RESET D:BREATH',
      font:'caption',color:0x294251ff});
  }
  const rebuildView=tx=>build(tx);
  const patchView=tx=>{
    const key=index*12+mode*2+(breathing?1:0);
    if(shown!==key){title.setText(tx,label());shown=key;}
  };
  globalThis.frame=function(buttons){
    idle();busy=true;
    try{
    const press=(buttons|0)&~held;held=buttons|0;
    if(press&0x4000){
      index=(index+1)%AyatsuriModels.names.length;tick=0;
    }
    if(press&0xa0){mode=(mode+(press&0x20?1:5))%6;tick=0;}
    if(press&0x10)tick=0;
    if(press&0x40){breathing=!breathing;tick=0;}
    if(diagnostics&&(press&0x40f0))diagnostics.reset();
    if(actor&&actorIndex!==index){actor.scene.dispose();actor=null;rebuild=true;}
    if(!actor){actor=AyatsuriLab.create(index,pocket.hone);actorIndex=index;}
    const sampleTick=diagnostics?diagnostics.sample(tick):tick;
    actor.evaluate(sampleTick*1000/30,mode,breathing);
    H.beginFrame(0xe77c);
    actor.scene.render();
    H.commit();
    tick=diagnostics?diagnostics.submitted(sampleTick,index,mode,breathing):tick+1;
    if(rebuild){V.replace(rebuildView);rebuild=false;}else V.patch(patchView);
    if(sampleTick%90===0){
      actor.scene.stats(stats);
      console.log('CHARACTER LAB model='+index+' test='+mode+' frame='+sampleTick+
        ' geometry='+stats.geometryUpdates+' shade='+stats.shadeUpdates);
    }
    }finally{busy=false;}
  };
}
