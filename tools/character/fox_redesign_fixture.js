// Host-only native lighting study. This actor has no legacy fox node bindings.
'use strict';
globalThis.FoxPatchStudy=(()=>{
  const data=globalThis.__foxCompiledData,H=pocket.hone,P=pocket.kasane.procedural,K=pocket.kasane,
    resource=P.resource({maxSegments:1024}),angles=new Float32Array(3),
    pose={position:[0,0,0],rotation:angles,scale:[1,1,1]},
    light={direction:[-.5,-.4,.8],color:0xffffff},
    camera={position:[0,0,400],target:[0,0,0],zoom:1.3,center:[120,87],projection:'orthographic'};
  let model=null,scene=null,root=null,shown=false,busy=false;
  function create(){
    if(scene)return;
    let nextModel=null,nextScene=null;
    try{
      nextModel=pocket.ayatsuri.modelBuiltin(data.asset);
      nextScene=AyatsuriNative.scene({model:nextModel,light,camera});
      const nextRoot=nextScene.handle(0);
      model=nextModel;scene=nextScene;root=nextRoot;
    }catch(error){if(nextScene)nextScene.dispose();if(nextModel)nextModel.dispose();throw error;}
  }
  function view(tx){tx.background(0xe4ece0ff);tx.image({resource,bounds:[0,0,240,135],sourceWidth:240,sourceHeight:135});}
  function draw(yaw,lightYaw=0){
    if(busy)throw Error('FoxRedesign: busy');
    if(typeof yaw!=='number'||!Number.isFinite(yaw)||Math.abs(yaw)>35||
       typeof lightYaw!=='number'||!Number.isFinite(lightYaw)||Math.abs(lightYaw)>180)throw RangeError('FoxRedesign: view range');
    busy=true;
    try{
      const a=lightYaw*Math.PI/180,c=Math.cos(a),s=Math.sin(a);
      light.direction[0]=-.5*c+.8*s;light.direction[2]=.5*s+.8*c;
      create();angles[1]=yaw*Math.PI/180;root.place(pose);scene.light(light);scene.evaluate(0);
      P.beginFrame(0xe77c);scene.render();P.commit();
      if(!shown){K.replace(view);shown=true;}
    }finally{busy=false;}
  }
  function stats(out){
    if(busy)throw Error('FoxRedesign: busy');
    out.vertices=data.vertices;out.triangles=data.triangles;out.fillCalls=data.meshes;
    out.nativePasses=1;out.culled=-1;
    // Fixture-owned typed storage only; native/utility memory is in guest counters.
    out.typedBytes=angles.byteLength;return out;
  }
  function release(){
    if(busy)throw Error('FoxRedesign: busy');busy=true;
    try{if(scene)scene.dispose();if(model)model.dispose();scene=model=root=null;}finally{busy=false;}
  }
  return {draw,stats,release};
})();
globalThis.__foxCompiledData=null;
