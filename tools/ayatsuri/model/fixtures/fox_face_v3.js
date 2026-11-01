// Fox-face v3 authoring. Units are head-local; +Y down, +Z toward the nose.
'use strict';
globalThis.FoxFaceModel=(()=>{
  const fur={color:0xe47b30,ambient:.38,diffuse:.62,specular:.025,roughness:.94,transmission:.08,transmissionColor:0xffbb70,thickness:.8},
    cream={color:0xffe8bb,ambient:.42,diffuse:.58,specular:.025,roughness:.92,transmission:.08,transmissionColor:0xffd6a0,thickness:.6},
    ink={color:0x39241c,ambient:.3,diffuse:.7,specular:.1,roughness:.7},
    noseMaterial={color:0x35221d,ambient:.26,diffuse:.55,specular:.68,roughness:.24},
    sclera={color:0xfff5e7,ambient:.45,diffuse:.55,specular:.16,roughness:.38},
    iris={color:0x713c24,ambient:.28,diffuse:.6,specular:.7,roughness:.16},
    pupil={color:0x211510,ambient:.2,diffuse:.4,specular:.82,roughness:.12},
    oral={color:0x54251f,ambient:.32,diffuse:.68,specular:.02,roughness:.9},
    earMaterial=Object.assign({},fur,{transmission:.48,transmissionColor:0xffb37a,thickness:.16,edgeSoftness:.08}),
    innerEarMaterial={color:0xe98974,ambient:.43,diffuse:.57,specular:.04,roughness:.86,transmission:.64,transmissionColor:0xffb697,thickness:.12,edgeSoftness:.08},
    tuftMaterial=Object.assign({},cream,{thickness:.22,transmission:.18,edgeSoftness:.08}),
    eyeXY=new Float32Array([-.9,0,-.6,-.62,0,-.88,.6,-.62,.9,0,.6,.5,0,.65,-.6,.5]);
  function finite(v){if(typeof v!=='number'||!Number.isFinite(v))throw new TypeError('FoxFace: finite value');return v;}
  function unit(v){finite(v);if(v<0||v>1)throw new RangeError('FoxFace: expression 0..1');return v;}
  function create(scene){
    if(typeof scene.aperture!=='function')throw new TypeError('FoxFace: optional clip scene required');
    const parts=[],eyes=[],outerEars=[],vertices=new Float32Array(24),scratch={position:[0,0,0],rotation:[0,0,0],scale:[1,1,1]};
    const root=scene.add({name:'fox-head',geometry:null});
    function group(name,parent,position,rotation){return scene.add({name,parent,geometry:null,position,rotation:rotation||[0,0,0]});}
    function ellipsoid(name,parent,position,radii,material,rotation){
      const h=scene.add({name,parent,position,rotation:rotation||[0,0,0],geometry:{kind:'ellipsoid',radii},material});parts.push(h);return h;
    }
    function volume(name,parent,position,lobes,blend,material){
      const h=scene.add({name,parent,position,geometry:{kind:'blendEllipsoids',lobes,blend},material});parts.push(h);return h;
    }
    function pose(h,x,y,z,rx,ry,rz,sx=1,sy=1,sz=1){
      const p=scratch.position,r=scratch.rotation,s=scratch.scale;
      p[0]=x;p[1]=y;p[2]=z;r[0]=rx;r[1]=ry;r[2]=rz;s[0]=sx;s[1]=sy;s[2]=sz;h.place(scratch);
    }
    function segment(handle,ax,ay,bx,by,z){
      const dx=bx-ax,dy=by-ay,length=Math.hypot(dx,dy);
      pose(handle,(ax+bx)*.5,(ay+by)*.5,z,0,0,Math.atan2(dy,dx),length*.52,1,1);
    }
    function clippedEye(parent,side){
      const globe=ellipsoid('sclera'+side,parent,[0,0,0],[8.4,8.5,1.1],sclera),
        color=ellipsoid('iris'+side,parent,[0,.2,1.0],[5.8,6.2,.55],iris),
        dark=ellipsoid('pupil'+side,parent,[0,.2,1.5],[2.35,4.1,.3],pupil),
        lidLeft=ellipsoid('lid-arc-left'+side,parent,[0,0,0],[1,.65,.55],fur),
        lidRight=ellipsoid('lid-arc-right'+side,parent,[0,0,0],[1,.65,.55],fur),
        brow=ellipsoid('brow'+side,parent,[0,-13,0],[7,1.15,1.2],ink);
      for(let i=0;i<8;i++){vertices[i*3]=eyeXY[i*2];vertices[i*3+1]=eyeXY[i*2+1]*.78;vertices[i*3+2]=.7;}
      // Unit-globe anchors share the fixed eye socket with both the trim and lids.
      const aperture=scene.aperture(globe,vertices);aperture.bind(globe).bind(color).bind(dark);
      return {aperture,update(smile,surprise){
        const closed=smile>=.98,height=Math.max(.03,.78*(1-smile)+.28*surprise),width=1+.04*surprise;
        for(let i=0;i<8;i++){
          vertices[i*3]=eyeXY[i*2]*width;vertices[i*3+1]=eyeXY[i*2+1]*height-.04*smile;vertices[i*3+2]=.7;
        }
        aperture.setVertices(vertices);aperture.setEmpty(closed?'closed':null);
        const left=vertices[0]*8.4,right=vertices[12]*8.4,
          corner=closed?.4:vertices[1]*10,peak=closed?-2.8:vertices[7]*10,z=vertices[2]*3;
        segment(lidLeft,left,corner,0,peak,z);segment(lidRight,0,peak,right,corner,z);
        pose(brow,0,-13-4*surprise+1.5*smile,0,0,0,-side*(.08+.12*smile-.12*surprise));
      }};
    }
    // Back-to-front authored order. This small slice does not claim global depth sorting.
    ellipsoid('skull',root,[0,-5,-5],[26,23,18],fur);
    for(let side=-1;side<=1;side+=2){
      // Broad outer shell; appended inner/tuft children preserve IDs 0..24.
      const outer=ellipsoid('ear-outer'+side,root,[side*23,-31,-7],[13,18,4],earMaterial,[0,0,side*.18]);
      outerEars.push(outer);
      scene.aperture(outer,new Float32Array([-.95,.65,.24,-.85,-.15,.24,side*.2,-.93,.24,.85,-.15,.24,.95,.65,.24])).bind(outer);
    }
    // The lower jaw extends back inside the head instead of ending at a plate.
    const jaw=group('jaw-hinge',root,[0,10,-1]);
    volume('lower-jaw',jaw,[0,0,0],[
      {position:[0,3,0],radii:[14,4.5,10]},
      {position:[0,4,9],radii:[11,3.5,8]},
      {position:[0,4,15],radii:[9,2.7,5]}],5,cream);
    // One material field joins the broad cheeks and forward muzzle continuously.
    const creamFace=volume('cream-face',root,[0,0,0],[
      {position:[0,8,19],radii:[10,6,8]},
      {position:[-18,6,4],radii:[14,7,9]},
      {position:[18,6,4],radii:[14,7,9]},
      {position:[-28,4,-1],radii:[5,2.8,4]},
      {position:[28,4,-1],radii:[5,2.8,4]},
      {position:[-10,7,13],radii:[10,6,8]},
      {position:[10,7,13],radii:[10,6,8]},
      {position:[0,11,2],radii:[15,6,10]}],6,cream);
    // The opening is confined to the supported lower-face area in this fixture.
    const mouth=ellipsoid('oral-volume',root,[0,15,22],[8,4,2],oral);
    volume('nose-bridge',root,[0,0,0],[
      {position:[0,-8,11],radii:[5,8,3.5]},
      {position:[0,0,18],radii:[3,5,3]}],3,fur);
    for(let side=-1;side<=1;side+=2){
      const eye=group('eye-socket'+side,root,[side*11.5,-4.5,11],[0,side*.12,side*.06]);
      eyes.push(clippedEye(eye,side));
    }
    const lip=ellipsoid('lip',root,[0,14.5,24],[8.8,.5,.7],ink);
    const nose=ellipsoid('nose',root,[0,6,27],[3.7,2.4,2.4],noseMaterial);
    // These last four authored nodes extend the frozen first 25 IDs.
    for(let k=0;k<2;k++){
      const side=k?1:-1,outer=outerEars[k],
        inner=ellipsoid('ear-inner'+side,outer,[0,-1,3.2],[8.8,12.8,.8],innerEarMaterial),
        tuft=ellipsoid('ear-tuft'+side,outer,[0,7,3.9],[6,7,1],tuftMaterial);
      for(const h of [inner,tuft])scene.aperture(h,new Float32Array([-.95,.65,.5,-.85,-.15,.5,side*.2,-.93,.5,.85,-.15,.5,.95,.65,.5])).bind(h);
    }
    function update(p){
      // Validate every independent control before any scene mutation.
      const yaw=finite(p.yaw===undefined?0:p.yaw),pitch=finite(p.pitch===undefined?0:p.pitch),
        smile=unit(p.smile===undefined?0:p.smile),surprise=unit(p.surprise===undefined?0:p.surprise),
        jawOpen=unit(p.jawOpen===undefined?0:p.jawOpen),open=Math.max(jawOpen,.68*smile,.45*surprise);
      pose(root,0,0,0,pitch,yaw,0);
      pose(creamFace,0,-1.8*smile,0,0,0,0,1+.04*smile,1,1);
      pose(jaw,0,10,-1,-.38*open,0,0,1+.08*smile-.14*surprise,1,1);
      mouth.visible(open>.015);
      pose(mouth,0,15+.5*open,22,-.12*open,0,0,1+.25*smile-.45*surprise,.15+.7*open+.4*surprise,1);
      pose(lip,0,14.5-1.1*smile,24,0,0,0,1+.2*smile,1,1);
      for(let k=0;k<2;k++){
        eyes[k].update(smile,surprise);
      }
    }
    update({});
    return {root,parts,eyes,nose,jaw,update,eyeClip:true,pointedEar:true,innerEar:true,tongue:false,convexNose:false};
  }
  return {create};
})();
