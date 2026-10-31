// HOST STUDY ONLY. Authored winding and centroid-depth ordering; no depth buffer.
// Baseline is fox_patch_fixture.js. Geometry comes from fox-retopology/study.json.
'use strict';
globalThis.FoxPatchStudy=(()=>{
  const variant=globalThis.__foxPatchVariant||'retopology';
  if(variant!=='retopology'&&variant!=='retopology-clay')throw RangeError('FoxPatch: retopology variant');
  const clay=variant==='retopology-clay';
  const H=pocket.hone,P=pocket.kasane.procedural,K=pocket.kasane,
    R=P.resource({maxSegments:1024}),light={direction:[-.5,-.4,.8],color:0xffffff},
    camera={position:[0,0,400],target:[0,0,0],zoom:1.3,center:[120,78],projection:'orthographic'},
    faces=46,vertexCount=30,eyeStart=vertexCount,browStart=eyeStart+16,
    mouthStart=browStart+6,pointCount=mouthStart+3,shadeCount=vertexCount,
    source=new Float32Array(pointCount*3),screen=new Float32Array(pointCount*2),
    normals=new Int16Array(shadeCount*3),restNormals=new Float32Array(vertexCount*3),
    indices=new Uint16Array([10,11,12,12,11,13,12,13,14,8,9,15,9,10,15,10,12,15,7,8,15,12,14,15,1,2,17,16,1,17,2,3,18,17,2,18,3,4,18,4,5,18,16,17,18,16,18,19,11,0,20,13,11,20,0,1,20,1,16,20,16,13,20,14,13,21,13,16,21,16,19,21,19,14,21,14,19,22,7,15,25,15,14,25,14,22,25,22,23,25,18,5,26,19,18,26,22,19,26,23,22,26,6,7,27,24,6,27,25,23,27,7,25,27,5,6,28,6,24,28,26,5,28,23,26,28,24,27,29,27,23,29,23,28,29,28,24,29]),colorsA=new Uint16Array(shadeCount),colorsB=new Uint16Array(shadeCount),
    albedoA=new Uint16Array(shadeCount),albedoB=new Uint16Array(shadeCount),thickness=new Uint8Array(shadeCount),
    lighting=new Float32Array([-.5,-.4,.8,0,0,1,1,1,1]),
    material=new Float32Array([.4,.6,.025,4,.08,1,.73,.44]),
    noseScreen=new Float32Array(8),noseNormal=new Float32Array(9),
    noseMaterial=new Float32Array([.26,.55,.68,64,0,1,1,1]),
    noseSurface=new Uint16Array([rgb565(0x35221d),rgb565(0x35221d),255,200,0]),
    noseTexture=new Float32Array([1,0,0,0,1,0,0,0,1]),
    matrix=new Float32Array(12),work=new Float32Array(40),
    lineOptions={closed:false},scratch={position:[0,0,0],rotation:[0,0,0],scale:[1,1,1]},
    bodyIds=[1,2,3,10,11,12,17,18,19,25,26,27,28];
  const patchXY=new Float32Array(screen.buffer,0,vertexCount*2),
    patchHoles=new Float32Array(work.buffer,32,32),
    patchCounts=new Uint8Array([8,8]),patchMask=new Float32Array(vertexCount),
    projectedDepth=new Float32Array(vertexCount);
  let selector=null,selected=null,asset=null,scene=null,handles=null,shown=false,fillCalls=0,culled=0,busy=false,
    yawValue=0,lightValue=0,prepared=false;
  function rgb565(v){return ((v>>>19)<<11)|(((v>>>10)&63)<<5)|((v>>>3)&31);}
  // Face-local barycentric interpolation is shared by all surface attachments.
  // This authored front sheet is single-valued in local XY; it is not a mesh query API.
  function depth(x,y){
    for(let k=0;k<indices.length;k+=3){
      const a=indices[k]*3,b=indices[k+1]*3,c=indices[k+2]*3,
        ux=source[b]-source[a],uy=source[b+1]-source[a+1],vx=source[c]-source[a],vy=source[c+1]-source[a+1],
        det=ux*vy-uy*vx,px=x-source[a],py=y-source[a+1],u=(px*vy-py*vx)/det,v=(ux*py-uy*px)/det;
      if(u>=-1e-7&&v>=-1e-7&&u+v<=1+1e-7)return source[a+2]+u*(source[b+2]-source[a+2])+v*(source[c+2]-source[a+2]);
    }
    throw RangeError('FoxPatch: attachment outside authored sheet');
  }
  function point(i,x,y,z){source[i*3]=x;source[i*3+1]=y;source[i*3+2]=z;}
  function onSurface(i,x,y){point(i,x,y,depth(x,y)+.15);}
  source.set(new Float32Array([0,-28,2,16,-24,1,25,-13,-2,31,0,-3,26,10,-2,13,18,-2,0,21,-3,-13,18,-2,-26,10,-2,-31,0,-3,-25,-13,-2,-16,-24,1,-18,-13,7,-4,-13,12,-4,3,16,-18,3,7,4,-13,12,18,-13,7,18,3,7,4,3,16,0,-18,14,0,-6,18,0,5,28,0,12,25,0,17,10,-8,8,23,8,8,23,-8,14,14,8,14,14,0,14.5,22]));
  // Each unnormalized cross product contributes twice its face area. Never flip normals.
  for(let k=0;k<indices.length;k+=3){
    const a=indices[k]*3,b=indices[k+1]*3,c=indices[k+2]*3,
      ux=source[b]-source[a],uy=source[b+1]-source[a+1],uz=source[b+2]-source[a+2],
      vx=source[c]-source[a],vy=source[c+1]-source[a+1],vz=source[c+2]-source[a+2],
      nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;
    if(!(Math.hypot(nx,ny,nz)>0))throw RangeError('FoxPatch: degenerate source triangle');
    for(let j=0;j<3;j++){const i=indices[k+j]*3;restNormals[i]+=nx;restNormals[i+1]+=ny;restNormals[i+2]+=nz;}
  }
  for(let i=0;i<vertexCount;i++){
    const j=i*3,length=Math.hypot(restNormals[j],restNormals[j+1],restNormals[j+2]);
    if(!(length>0))throw RangeError('FoxPatch: undefined vertex normal');
    restNormals[j]/=length;restNormals[j+1]/=length;restNormals[j+2]/=length;
    patchMask[i]=source[j+1]+.3*Math.abs(source[j])-5;
  }
  const ring=[-6,-.5,-4,-5.5,0,-7,4,-5.5,6,-.5,4,3,0,4,-4,3];
  for(let side=0;side<2;side++){
    const x=side?11:-11,y=-5;
    // Rim points use the same authored face interpolation as the visible sheet.
    for(let k=0;k<8;k++){const u=ring[k*2],v=ring[k*2+1];point(eyeStart+side*8+k,x+u,y+v,depth(x+u,y+v));}
    onSurface(browStart+side*3,x-6,-16);onSurface(browStart+side*3+1,x,-18);onSurface(browStart+side*3+2,x+6,-16);
  }
  onSurface(mouthStart,-6,11);onSurface(mouthStart+1,0,12);onSurface(mouthStart+2,6,11);
  albedoA.fill(rgb565(clay?0xcbbca6:0xe47b30));albedoB.fill(rgb565(clay?0xcbbca6:0xffe8bb));thickness.fill(175);
  function pose(handle,x,y,z,ry=0,sx=1,sy=1,sz=1){
    scratch.position[0]=x;scratch.position[1]=y;scratch.position[2]=z;
    scratch.rotation[0]=scratch.rotation[2]=0;scratch.rotation[1]=ry;
    scratch.scale[0]=sx;scratch.scale[1]=sy;scratch.scale[2]=sz;handle.place(scratch);
  }
  function create(){
    if(scene)return;
    let nextAsset=null,nextScene=null;
    try{
      nextAsset=pocket.ayatsuri.modelBuiltin('fox-face');nextScene=AyatsuriClip.nativeScene({model:nextAsset,light,camera});
      const h=[];for(let i=0;i<29;i++)h.push(nextScene.handle(i));
      for(let k=0;k<2;k++){
        const side=k?1:-1,outer=h[2+k],inner=h[25+k*2],tuft=h[26+k*2];
        nextScene.aperture(outer,new Float32Array([-.94,.7,.25,-.88,-.15,.25,side*.12,-.95,.25,.88,-.15,.25,.94,.7,.25])).bind(outer);
        nextScene.aperture(inner,new Float32Array([-.88,.3,.5,-.82,-.1,.5,side*.1,-.94,.5,.82,-.1,.5,.88,.3,.5])).bind(inner);
        nextScene.aperture(tuft,new Float32Array([-.8,.65,.45,-.72,-.45,.45,side*.08,-.98,.45,.72,-.45,.45,.8,.65,.45])).bind(tuft);
        pose(h[k?16:9],side*11,-5,depth(side*11,-5)-.2);
        {
          pose(h[k?18:11],0,.9,2.6,0,1.15,1.12,1);
          pose(h[k?19:12],0,.9,3.4,0,1.1,1.05,1);
        }
      }
      for(let i=0;i<29;i++)h[i].visible(i===0||i===9||i===16||bodyIds.indexOf(i)>=0);
      asset=nextAsset;scene=nextScene;handles=h;
    }catch(error){if(nextScene)nextScene.dispose();if(nextAsset)nextAsset.dispose();throw error;}
  }
  function convex(start){
    let sign=0;
    for(let k=0;k<8;k++){
      const a=(start+k)*2,b=(start+(k+1)%8)*2,c=(start+(k+2)%8)*2,
        cross=(screen[b]-screen[a])*(screen[c+1]-screen[b+1])-(screen[b+1]-screen[a+1])*(screen[c]-screen[b]);
      if(!Number.isFinite(cross)||Math.abs(cross)<.01||(sign&&cross*sign<=0))throw Error('FoxPatch: folded aperture');
      sign=cross>0?1:-1;
    }
  }
  function prepare(yaw,lightYaw){
    if(typeof yaw!=='number'||!Number.isFinite(yaw)||Math.abs(yaw)>35||
       typeof lightYaw!=='number'||!Number.isFinite(lightYaw)||Math.abs(lightYaw)>180)throw RangeError('FoxPatch: view range');
    const a=Math.fround(yaw*Math.PI/180),c=Math.fround(Math.cos(a)),s=Math.fround(Math.sin(a)),
      zoom=Math.fround(1.3),l=lightYaw*Math.PI/180,lc=Math.cos(l),ls=Math.sin(l);
    lighting[0]=light.direction[0]=-.5*lc+.8*ls;lighting[1]=-.4;lighting[2]=light.direction[2]=.5*ls+.8*lc;
    matrix[0]=zoom*c;matrix[1]=0;matrix[2]=zoom*s;matrix[3]=120;
    matrix[4]=0;matrix[5]=zoom;matrix[6]=0;matrix[7]=78;
    matrix[8]=matrix[9]=matrix[10]=0;matrix[11]=1;H.project(matrix,source,screen);convex(eyeStart);convex(eyeStart+8);
    for(let i=0;i<vertexCount;i++)projectedDepth[i]=-s*source[i*3]+c*source[i*3+2];
    if(!selector)selector=AyatsuriVisibility.create({vertexCount,indices,order:'centroid-depth'});
    selected=selector.evaluate(patchXY,projectedDepth);
    const noseZ=depth(0,6)+1;
    noseScreen[0]=zoom*c*3.7;noseScreen[1]=0;noseScreen[2]=zoom*s*2.4;noseScreen[3]=120+zoom*s*noseZ;
    noseScreen[4]=0;noseScreen[5]=zoom*2.4;noseScreen[6]=0;noseScreen[7]=78+zoom*6;
    noseNormal[0]=c/3.7;noseNormal[1]=0;noseNormal[2]=s/2.4;
    noseNormal[3]=0;noseNormal[4]=1/2.4;noseNormal[5]=0;
    noseNormal[6]=-s/3.7;noseNormal[7]=0;noseNormal[8]=c/2.4;
    for(let k=0;k<shadeCount;k++){
      const x=restNormals[k*3],y=restNormals[k*3+1],z=restNormals[k*3+2];
      const length=Math.hypot(x,y,z);normals[k*3]=Math.round((c*x+s*z)*32767/length);
      normals[k*3+1]=Math.round(y*32767/length);normals[k*3+2]=Math.round((-s*x+c*z)*32767/length);
    }
    H.shade(normals,albedoA,thickness,lighting,material,colorsA);H.shade(normals,albedoB,thickness,lighting,material,colorsB);
    for(let k=0;k<32;k++)work[8+k]=screen[eyeStart*2+k];
    yawValue=a;lightValue=lightYaw;prepared=true;
  }
  function layer(){
    // Bound native clip anchors stay visible. The nose uses an existing
    // analytic primitive after the patch, avoiding another instance/evaluate.
    pose(handles[0],0,0,0,yawValue);scene.light(light);scene.evaluate(0);scene.render();
  }
  function view(tx){tx.background(0xe4ece0ff);tx.image({resource:R,bounds:[0,0,240,135],sourceWidth:240,sourceHeight:135});}
  function draw(yaw,lightYaw=0){
    if(busy)throw Error('FoxPatch: busy');busy=true;try{
    prepare(yaw,lightYaw);create();fillCalls=culled=0;P.beginFrame(0xe77c);layer();
    // The selector returns borrowed ordered triples; the native packet snapshots them.
    // Centroid ordering is only a bounded study heuristic, not general occlusion.
    culled=faces-selected.triangleCount;
    H.gouraudPatch(patchXY,selected.indices,colorsA,colorsB,patchMask,patchHoles,patchCounts,selected.triangleCount);fillCalls=1;
    H.ellipsoid(noseScreen,noseNormal,lighting,noseMaterial,noseSurface,noseTexture);
    H.outline(screen,eyeStart,5,rgb565(0x39241c),lineOptions);H.outline(screen,eyeStart+8,5,rgb565(0x39241c),lineOptions);
    H.outline(screen,browStart,3,rgb565(0x39241c),lineOptions);H.outline(screen,browStart+3,3,rgb565(0x39241c),lineOptions);
    H.outline(screen,mouthStart,3,rgb565(0x39241c),lineOptions);P.commit();if(!shown){K.replace(view);shown=true;}
    }finally{busy=false;}
  }
  function stats(out){
    if(busy)throw Error('FoxPatch: busy');
    out.vertices=vertexCount;out.triangles=faces;out.fillCalls=fillCalls;out.culled=culled;out.nativePasses=1;
    out.typedBytes=source.byteLength+screen.byteLength+normals.byteLength+restNormals.byteLength+indices.byteLength+
      colorsA.byteLength+colorsB.byteLength+albedoA.byteLength+albedoB.byteLength+thickness.byteLength+
      lighting.byteLength+material.byteLength+matrix.byteLength+work.byteLength+noseScreen.byteLength+noseNormal.byteLength+
      noseMaterial.byteLength+noseSurface.byteLength+noseTexture.byteLength;
    out.typedBytes+=patchMask.byteLength+patchCounts.byteLength+projectedDepth.byteLength;
    // Selector steady storage: copied source, output capacity, and three counters.
    if(selector)out.typedBytes+=indices.byteLength*2+12;
    out.visibleTriangles=faces-culled;
    out.prepared=prepared;out.lightYaw=lightValue;return out;
  }
  function release(){if(busy)throw Error('FoxPatch: busy');if(scene)scene.dispose();if(asset)asset.dispose();scene=asset=handles=null;if(selector)selector.dispose();selector=selected=null;prepared=false;}
  return {draw,stats,release};
})();
