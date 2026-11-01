// Neutral native face viewer. Model/Instance own geometry; this app helper owns its display resource.
'use strict';
globalThis.FoxSheetScene=Object.freeze({create(data,options){
  const present=options===undefined?false:options.present;
  if(options!==undefined&&(typeof options!=='object'||options===null||typeof present!=='boolean'))
    throw TypeError('FoxSheet: explicit boolean present option required');
  if(!data||typeof data.asset!=='string'||!(data.extra instanceof Float32Array)||data.extra.length!==75||
     !Number.isFinite(data.noseZ)||!data.ears||data.ears.length!==2)throw TypeError('FoxCompiled: selected offline metadata required');
  const assetName=data.asset,extra=new Float32Array(data.extra),noseZ=data.noseZ,
    ears=data.ears.map(e=>({side:e.side,outer:e.outer,inner:e.inner,tuft:e.tuft}));data=null;
  const H=pocket.hone,P=pocket.kasane.procedural,K=pocket.kasane,
    resource=P.resource({maxSegments:1024}),light={direction:[-.5,-.4,.8],color:0xffffff},
    camera={position:[0,0,400],target:[0,0,0],zoom:Math.fround(1.3),center:[120,78],projection:'orthographic'},
    matrix=new Float32Array(12),screen=new Float32Array(50),noseScreen=new Float32Array(8),noseNormal=new Float32Array(9),
    lighting=new Float32Array([-.5,-.4,.8,0,0,1,1,1,1]),
    noseMaterial=new Float32Array([.26,.55,.68,64,0,1,1,1]),
    noseSurface=new Uint16Array([rgb565(0x35221d),rgb565(0x35221d),255,200,0]),
    noseTexture=new Float32Array([1,0,0,0,1,0,0,0,1]),lineOptions={closed:false},
    pose={position:[0,0,0],rotation:[0,0,0],scale:[1,1,1]};
  let asset=null,scene=null,root=null,shown=false,busy=false,prepared=false,yawValue=0,lightValue=0;
  function rgb565(v){return ((v>>>19)<<11)|(((v>>>10)&63)<<5)|((v>>>3)&31);}
  function create(){
    if(scene)return;
    let nextAsset=null,nextScene=null;
    try{
      nextAsset=pocket.ayatsuri.modelBuiltin(assetName);nextScene=AyatsuriClip.nativeScene({model:nextAsset,light,camera});
      // Eye rest positions and fixed primitive visibility are already in the asset.
      for(let k=0;k<2;k++){
        const e=ears[k],s=e.side,outer=nextScene.handle(e.outer),inner=nextScene.handle(e.inner),tuft=nextScene.handle(e.tuft);
        nextScene.aperture(outer,new Float32Array([-.94,.7,.25,-.88,-.15,.25,s*.12,-.95,.25,.88,-.15,.25,.94,.7,.25])).bind(outer);
        nextScene.aperture(inner,new Float32Array([-.88,.3,.5,-.82,-.1,.5,s*.1,-.94,.5,.82,-.1,.5,.88,.3,.5])).bind(inner);
        nextScene.aperture(tuft,new Float32Array([-.8,.65,.45,-.72,-.45,.45,s*.08,-.98,.45,.72,-.45,.45,.8,.65,.45])).bind(tuft);
      }
      const nextRoot=nextScene.handle(0);
      asset=nextAsset;scene=nextScene;root=nextRoot;
    }catch(error){if(nextScene)nextScene.dispose();if(nextAsset)nextAsset.dispose();throw error;}
  }
  function convex(start){
    let sign=0;
    for(let k=0;k<8;k++){
      const a=(start+k)*2,b=(start+(k+1)%8)*2,c=(start+(k+2)%8)*2,
        cross=(screen[b]-screen[a])*(screen[c+1]-screen[b+1])-(screen[b+1]-screen[a+1])*(screen[c]-screen[b]);
      if(!Number.isFinite(cross)||Math.abs(cross)<.01||(sign&&cross*sign<=0))throw Error('FoxCompiled: folded aperture');
      sign=cross>0?1:-1;
    }
  }
  function prepare(yaw,lightYaw){
    if(typeof yaw!=='number'||!Number.isFinite(yaw)||Math.abs(yaw)>35||
       typeof lightYaw!=='number'||!Number.isFinite(lightYaw)||Math.abs(lightYaw)>180)throw RangeError('FoxCompiled: view range');
    const a=Math.fround(yaw*Math.PI/180),c=Math.fround(Math.cos(a)),s=Math.fround(Math.sin(a)),zoom=Math.fround(1.3),
      l=lightYaw*Math.PI/180,lc=Math.cos(l),ls=Math.sin(l);
    lighting[0]=light.direction[0]=-.5*lc+.8*ls;lighting[1]=-.4;lighting[2]=light.direction[2]=.5*ls+.8*lc;
    matrix[0]=zoom*c;matrix[1]=0;matrix[2]=zoom*s;matrix[3]=120;
    matrix[4]=0;matrix[5]=zoom;matrix[6]=0;matrix[7]=78;matrix[8]=matrix[9]=matrix[10]=0;matrix[11]=1;
    H.project(matrix,extra,screen);convex(0);convex(8);
    noseScreen[0]=zoom*c*3.7;noseScreen[1]=0;noseScreen[2]=zoom*s*2.4;noseScreen[3]=120+zoom*s*noseZ;
    noseScreen[4]=0;noseScreen[5]=zoom*2.4;noseScreen[6]=0;noseScreen[7]=78+zoom*6;
    noseNormal[0]=c/3.7;noseNormal[1]=0;noseNormal[2]=s/2.4;
    noseNormal[3]=0;noseNormal[4]=1/2.4;noseNormal[5]=0;
    noseNormal[6]=-s/3.7;noseNormal[7]=0;noseNormal[8]=c/2.4;
    yawValue=a;lightValue=lightYaw;prepared=true;
  }
  function view(tx){tx.background(0xe4ece0ff);tx.image({resource,bounds:[0,0,240,135],sourceWidth:240,sourceHeight:135});}
  function draw(yaw,lightYaw=0){
    if(busy)throw Error('FoxCompiled: busy');busy=true;
    try{
      prepare(yaw,lightYaw);create();pose.rotation[1]=yawValue;root.place(pose);scene.light(light);scene.evaluate(0);
      P.beginFrame(0xe77c);scene.render();
      H.ellipsoid(noseScreen,noseNormal,lighting,noseMaterial,noseSurface,noseTexture);
      H.outline(screen,0,5,rgb565(0x39241c),lineOptions);H.outline(screen,8,5,rgb565(0x39241c),lineOptions);
      H.outline(screen,16,3,rgb565(0x39241c),lineOptions);H.outline(screen,19,3,rgb565(0x39241c),lineOptions);
      H.outline(screen,22,3,rgb565(0x39241c),lineOptions);P.commit();
      if(present&&!shown){K.replace(view);shown=true;}
    }finally{busy=false;}
  }
  function stats(out){
    if(busy)throw Error('FoxCompiled: busy');
    out.vertices=30;out.triangles=46;out.fillCalls=1;out.nativePasses=1;
    // Selection lives in the native instance. No duplicate JS mesh for counters.
    out.culled=-1;out.visibleTriangles=-1;out.prepared=prepared;out.lightYaw=lightValue;
    out.typedBytes=extra.byteLength+matrix.byteLength+screen.byteLength+noseScreen.byteLength+noseNormal.byteLength+
      lighting.byteLength+noseMaterial.byteLength+noseSurface.byteLength+noseTexture.byteLength;
    return out;
  }
  function release(){
    if(busy)throw Error('FoxCompiled: busy');busy=true;
    try{if(scene)scene.dispose();if(asset)asset.dispose();scene=asset=root=null;prepared=false;}
    finally{busy=false;}
  }
  return {draw,stats,release,view};
}});
