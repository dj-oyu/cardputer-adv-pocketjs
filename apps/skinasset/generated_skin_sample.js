'use strict';
// Public two-pose fixture adapter, not a general rig player or interpolator.
globalThis.GeneratedSkinSample={start(blob,asset){
  function array(value,Type,length){if(!(value instanceof Type)||value.length!==length)throw Error('fixture array contract');}
  if(!asset||!Array.isArray(asset.samples)||asset.samples.length!==2)throw Error('two fixture poses required');
  if(asset.samples[0].name!=='rest'||asset.samples[1].name!=='child_y6')throw Error('fixture sample order');
  array(asset.V,Float32Array,36);array(asset.IB,Float32Array,24);
  array(asset.J,Uint16Array,24);array(asset.W,Uint16Array,24);
  for(const sample of asset.samples)array(sample.G,Float32Array,24);
  const P=pocket.kasane.shiki;let model=null,scene=null,stopped=false,viewBack=false;
  try{model=pocket.ayatsuri.modelOpen(blob);scene=AyatsuriNative.scene({model,
    camera:{position:[200,0,400],target:[0,0,0],zoom:1,center:[120,67]},
    light:{direction:[0,0,1],color:0xffffff}});
    const app={evaluate(timeMs,back=false){
      if(stopped)throw Error('generated skin sample stopped');
      if(typeof timeMs!=='number'||!Number.isFinite(timeMs)||timeMs<0)throw Error('nonnegative finite ms required');
      if(typeof back!=='boolean')throw Error('boolean view required');
      // A step selector only: sample 0 before 1000ms, sample 1 thereafter.
      // G and IB already mesh-local; no additional basis or node transform here.
      if(back!==viewBack){scene.camera({position:[back?-200:200,0,back?-400:400]});viewBack=back;}
      scene.evaluateSkin(timeMs,0,asset.samples[timeMs<1000?0:1].G,asset.IB,asset.V,asset.J,asset.W);
    },render(){if(stopped)throw Error('generated skin sample stopped');P.beginMeshFrame(17,2048,200000,1);scene.render();P.commit();},
    frame(timeMs){app.evaluate(timeMs);app.render();},
    stats(out){if(stopped)throw Error('generated skin sample stopped');return scene.stats(out);},
    stop(){if(stopped)return;scene.dispose();model.dispose();stopped=true;scene=model=asset=null;}};
    return app;
  }catch(error){if(scene)scene.dispose();if(model)model.dispose();throw error;}
}};
