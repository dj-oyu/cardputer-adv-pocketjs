// HOST STUDY ONLY. Existing native primitives; no new renderer or depth buffer.
'use strict';
globalThis.FoxPatchStudy=(()=>{
  const variant=globalThis.__foxPatchVariant||'baseline';
  if(['baseline','unlit','clay','dense-clay','dense','socket','socket-only','gouraud','gouraud-socket'].indexOf(variant)<0)throw RangeError('FoxPatch: variant');
  const dense=variant==='dense-clay'||variant==='dense'||variant==='socket',
    clay=variant==='clay'||variant==='dense-clay',socket=variant==='socket'||variant==='socket-only'||variant==='gouraud-socket',
    gouraud=variant==='gouraud'||variant==='gouraud-socket';
  const H=pocket.hone,P=pocket.kasane.procedural,K=pocket.kasane,
    R=P.resource({maxSegments:1024}),light={direction:[-.5,-.4,.8],color:0xffffff},
    camera={position:[0,0,400],target:[0,0,0],zoom:1.3,center:[120,78],projection:'orthographic'},
    ys=dense?[-27,-21,-14,-7,0,7,13,17]:[-27,-17,-7,3,11,17],
    widths=ys.map(y=>26*Math.sqrt(1-((y+5)/23)**2)),cols=dense?7:5,rows=ys.length,
    faces=(rows-1)*(cols-1)*2,vertexCount=rows*cols,eyeStart=vertexCount,browStart=eyeStart+16,
    mouthStart=browStart+6,pointCount=mouthStart+3,shadeCount=gouraud?vertexCount:faces,
    source=new Float32Array(pointCount*3),screen=new Float32Array(pointCount*2),
    normals=new Int16Array(shadeCount*3),restNormals=new Float32Array(vertexCount*3),
    indices=new Uint16Array(faces*3),colorsA=new Uint16Array(shadeCount),colorsB=new Uint16Array(shadeCount),
    albedoA=new Uint16Array(shadeCount),albedoB=new Uint16Array(shadeCount),thickness=new Uint8Array(shadeCount),
    lighting=new Float32Array([-.5,-.4,.8,0,0,1,1,1,1]),
    material=new Float32Array([.4,.6,.025,4,.08,1,.73,.44]),
    noseScreen=new Float32Array(8),noseNormal=new Float32Array(9),
    noseMaterial=new Float32Array([.26,.55,.68,64,0,1,1,1]),
    noseSurface=new Uint16Array([rgb565(0x35221d),rgb565(0x35221d),255,200,0]),
    noseTexture=new Float32Array([1,0,0,0,1,0,0,0,1]),
    matrix=new Float32Array(12),work=new Float32Array(40),holes=[4,8,12,8],polygon=[0,3],
    lineOptions={closed:false},scratch={position:[0,0,0],rotation:[0,0,0],scale:[1,1,1]},
    bodyIds=[1,2,3,10,11,12,17,18,19,25,26,27,28];
  const patchXY=gouraud?new Float32Array(screen.buffer,0,vertexCount*2):null,
    patchHoles=gouraud?new Float32Array(work.buffer,32,32):null,
    patchCounts=gouraud?new Uint8Array([8,8]):null,patchMask=gouraud?new Float32Array(vertexCount):null;
  let asset=null,scene=null,handles=null,shown=false,fillCalls=0,culled=0,busy=false,
    yawValue=0,lightValue=0,prepared=false;
  function rgb565(v){return ((v>>>19)<<11)|(((v>>>10)&63)<<5)|((v>>>3)&31);}
  function depth(x,y){
    const cross=Math.max(.001,1-((y+5)/23)**2),width=26*Math.sqrt(cross),
      dome=-5+18*Math.sqrt(Math.max(0,cross-(x/26)**2)),
      t=Math.max(0,Math.min(1,(y+27)/44)),fade=Math.max(0,1-(x/width)**2)**2*Math.sin(Math.PI*t)**2,
      muzzle=11*Math.exp(-((x/10)**2)-((y-9)/10)**2),
      socket=3.5*Math.exp(-(((Math.abs(x)-11)/6)**2)-((y+5)/7)**2);
    return dome+fade*(muzzle-socket);
  }
  function dx(x,y){return (depth(x+.05,y)-depth(x-.05,y))*10;}
  function dy(x,y){return (depth(x,y+.05)-depth(x,y-.05))*10;}
  function point(i,x,y,z){source[i*3]=x;source[i*3+1]=y;source[i*3+2]=z;}
  function onSurface(i,x,y){point(i,x,y,depth(x,y)+.15);}
  for(let r=0;r<rows;r++)for(let c=0;c<cols;c++){
    const i=r*cols+c,x=(c-(cols-1)*.5)*widths[r]/((cols-1)*.5),y=ys[r],nx=-dx(x,y),ny=-dy(x,y),length=Math.hypot(nx,ny,1);
    point(i,x,y,depth(x,y));restNormals[i*3]=nx/length;restNormals[i*3+1]=ny/length;restNormals[i*3+2]=1/length;
    if(gouraud)patchMask[i]=y+.3*Math.abs(x)-5;
  }
  let f=0;
  for(let r=0;r<rows-1;r++)for(let c=0;c<cols-1;c++){
    const a=r*cols+c,b=a+1,d=a+cols,e=d+1;
    indices[f++]=a;indices[f++]=b;indices[f++]=d;
    indices[f++]=b;indices[f++]=e;indices[f++]=d;
  }
  const ring=socket?[-6,-.5,-4,-5.5,0,-7,4,-5.5,6,-.5,4,3,0,4,-4,3]:[-6,-3,-4,-7,0,-8,4,-7,6,-3,5,3,0,5,-5,3];
  for(let side=0;side<2;side++){
    const x=side?11:-11,y=-5,z=depth(x,y),gx=dx(x,y),gy=dy(x,y);
    // Convex apertures lie on the local tangent plane, not separate yaw drawings.
    for(let k=0;k<8;k++){const u=ring[k*2],v=ring[k*2+1];point(eyeStart+side*8+k,x+u,y+v,z+gx*u+gy*v);}
    onSurface(browStart+side*3,x-6,-16);onSurface(browStart+side*3+1,x,-18);onSurface(browStart+side*3+2,x+6,-16);
  }
  onSurface(mouthStart,-6,11);onSurface(mouthStart+1,0,12);onSurface(mouthStart+2,6,11);
  albedoA.fill(rgb565(clay?0xcbbca6:0xe47b30));albedoB.fill(rgb565(clay?0xcbbca6:0xffe8bb));thickness.fill(175);
  if(variant==='unlit'){material[0]=1;material[1]=material[2]=material[4]=0;}
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
        if(socket){
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
    const noseZ=depth(0,6)+1;
    noseScreen[0]=zoom*c*3.7;noseScreen[1]=0;noseScreen[2]=zoom*s*2.4;noseScreen[3]=120+zoom*s*noseZ;
    noseScreen[4]=0;noseScreen[5]=zoom*2.4;noseScreen[6]=0;noseScreen[7]=78+zoom*6;
    noseNormal[0]=c/3.7;noseNormal[1]=0;noseNormal[2]=s/2.4;
    noseNormal[3]=0;noseNormal[4]=1/2.4;noseNormal[5]=0;
    noseNormal[6]=-s/3.7;noseNormal[7]=0;noseNormal[8]=c/2.4;
    for(let k=0;k<shadeCount;k++){
      let x=0,y=0,z=0;
      if(gouraud){x=restNormals[k*3];y=restNormals[k*3+1];z=restNormals[k*3+2];}
      else for(let j=0;j<3;j++){const i=indices[k*3+j]*3;x+=restNormals[i];y+=restNormals[i+1];z+=restNormals[i+2];}
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
  function paint(k,cream){
    // Clip one convex triangle against its body-local albedo half-plane.
    // The grid has a central column, so each triangle stays in one hemisphere.
    let count=0;
    for(let j=0;j<3;j++){
      const ai=indices[k*3+j],bi=indices[k*3+(j+1)%3],
        af=source[ai*3+1]+.3*Math.abs(source[ai*3])-5,bf=source[bi*3+1]+.3*Math.abs(source[bi*3])-5,
        a=cream?af:-af,b=cream?bf:-bf;
      if(a>=0){work[count*2]=screen[ai*2];work[count*2+1]=screen[ai*2+1];++count;}
      if((a>=0)!==(b>=0)){
        const t=a/(a-b);if(t>0&&t<1){work[count*2]=screen[ai*2]+t*(screen[bi*2]-screen[ai*2]);
        work[count*2+1]=screen[ai*2+1]+t*(screen[bi*2+1]-screen[ai*2+1]);++count;}
      }
    }
    if(count>=3){polygon[1]=count;H.fill(work,polygon,holes,cream?colorsB[k]:colorsA[k]);++fillCalls;}
  }
  function view(tx){tx.background(0xe4ece0ff);tx.image({resource:R,bounds:[0,0,240,135],sourceWidth:240,sourceHeight:135});}
  function draw(yaw,lightYaw=0){
    if(busy)throw Error('FoxPatch: busy');busy=true;try{
    prepare(yaw,lightYaw);create();fillCalls=culled=0;P.beginFrame(0xe77c);layer();
    if(gouraud){H.gouraudPatch(patchXY,indices,colorsA,colorsB,patchMask,patchHoles,patchCounts);fillCalls=1;}
    else for(let k=0;k<faces;k++){
      const a=indices[k*3]*2,b=indices[k*3+1]*2,d=indices[k*3+2]*2,
        area=(screen[b]-screen[a])*(screen[d+1]-screen[a+1])-(screen[b+1]-screen[a+1])*(screen[d]-screen[a]);
      if(area<=.01){++culled;continue;}paint(k,false);paint(k,true);
    }
    H.ellipsoid(noseScreen,noseNormal,lighting,noseMaterial,noseSurface,noseTexture);
    if(socket){H.outline(screen,eyeStart,5,rgb565(0x39241c),lineOptions);H.outline(screen,eyeStart+8,5,rgb565(0x39241c),lineOptions);}
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
    if(gouraud)out.typedBytes+=patchMask.byteLength+patchCounts.byteLength;
    out.prepared=prepared;out.lightYaw=lightValue;return out;
  }
  function release(){if(busy)throw Error('FoxPatch: busy');if(scene)scene.dispose();if(asset)asset.dispose();scene=asset=handles=null;prepared=false;}
  return {draw,stats,release};
})();
