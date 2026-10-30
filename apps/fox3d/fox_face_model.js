// Expressions over fixed native fox-face nodes. No JS shape/material graph.
'use strict';
globalThis.FoxFaceModel=(()=>{
  // The upper rim covers part of the globe; the lower opening stays rounded.
  const eyeXY=new Float32Array([-1,-.25,-.55,-.7,.55,-.7,1,-.25,1,.4,.5,.76,-.5,.76,-1,.4]);
  function finite(v){if(typeof v!=='number'||!Number.isFinite(v))throw new TypeError('FoxFace: finite value');return v;}
  function unit(v){finite(v);if(v<0||v>1)throw new RangeError('FoxFace: expression 0..1');return v;}
  function create(scene){
    if(typeof scene.aperture!=='function'||typeof scene.handle!=='function')throw new TypeError('FoxFace: native clip scene required');
    const h=[];for(let i=0;i<29;i++)h.push(scene.handle(i));
    const root=h[0],jaw=h[4],creamFace=h[6],mouth=h[7],lip=h[23],nose=h[24],eyes=[],
      vertices=new Float32Array(24),scratch={position:[0,0,0],rotation:[0,0,0],scale:[1,1,1]};
    function pose(handle,x,y,z,rx,ry,rz,sx=1,sy=1,sz=1){
      const p=scratch.position,r=scratch.rotation,s=scratch.scale;
      p[0]=x;p[1]=y;p[2]=z;r[0]=rx;r[1]=ry;r[2]=rz;s[0]=sx;s[1]=sy;s[2]=sz;handle.place(scratch);
    }
    function segment(handle,ax,ay,bx,by,z,thickness=1){
      const dx=bx-ax,dy=by-ay,length=Math.hypot(dx,dy);
      pose(handle,(ax+bx)*.5,(ay+by)*.5,z,0,0,Math.atan2(dy,dx),length*.52,thickness,1);
    }
    for(let i=0;i<2;i++){
      const side=i?1:-1,outer=h[2+i];
      scene.aperture(outer,new Float32Array([-.94,.7,.25,-.88,-.15,.25,side*.12,-.95,.25,.88,-.15,.25,.94,.7,.25])).bind(outer);
      const inner=h[25+i*2],tuft=h[26+i*2];
      // Keep appended inner-ear paint above the forehead; the root tuft is foreground.
      scene.aperture(inner,new Float32Array([-.88,.3,.5,-.82,-.1,.5,side*.1,-.94,.5,.82,-.1,.5,.88,.3,.5])).bind(inner);
      scene.aperture(tuft,new Float32Array([-.8,.65,.45,-.72,-.45,.45,side*.08,-.98,.45,.72,-.45,.45,.8,.65,.45])).bind(tuft);
    }
    function eye(side,id){
      const globe=h[id],color=h[id+1],dark=h[id+2],lidLeft=h[id+3],lidRight=h[id+4],brow=h[id+5];
      const material={color:0},openColor=0xc56b32,closedColor=0x39241c;let lastColor=-1;
      for(let i=0;i<8;i++){vertices[i*3]=eyeXY[i*2]*.95;vertices[i*3+1]=eyeXY[i*2+1]*.86;vertices[i*3+2]=.55;}
      const aperture=scene.aperture(globe,vertices);aperture.bind(globe).bind(color).bind(dark);
      return {aperture,update(smile,surprise,viewYaw){
        const closed=smile>=.98,height=Math.max(.03,.86*(1-smile)+.14*surprise),
          far=Math.max(0,side*Math.sin(viewYaw)),width=(.95+.05*surprise)*(1-.4*far),shift=-side*.12*far;
        for(let i=0;i<8;i++){
          vertices[i*3]=eyeXY[i*2]*width+shift;vertices[i*3+1]=eyeXY[i*2+1]*height-.04*smile;vertices[i*3+2]=.55;
        }
        aperture.setVertices(vertices);aperture.setEmpty(closed?'closed':null);
        const arc=Math.max(0,Math.min(1,(smile-.7)/.28)),endpoint=width*8.4,
          corner=closed?.4:(1-arc)*(-.25*height*8.5-.4*smile)+arc*.4,
          peak=closed?-2.8:(1-arc)*(-.72*height*8.5-.4*smile)-arc*2.8,z=1,
          color=smile>.7?closedColor:openColor;
        if(color!==lastColor){material.color=color;lidLeft.material(material);lidRight.material(material);lastColor=color;}
        lidLeft.visible(true);lidRight.visible(true);
        segment(lidLeft,-endpoint+shift*8.4,corner,shift*8.4,peak,z,.75);
        segment(lidRight,shift*8.4,peak,endpoint+shift*8.4,corner,z,.75);
        pose(brow,0,-13-4*surprise+1.5*smile,0,0,0,-side*(.08+.12*smile-.12*surprise));
      }};
    }
    eyes.push(eye(-1,10));eyes.push(eye(1,17));
    function update(p){
      const yaw=finite(p.yaw===undefined?0:p.yaw),pitch=finite(p.pitch===undefined?0:p.pitch),
        smile=unit(p.smile===undefined?0:p.smile),surprise=unit(p.surprise===undefined?0:p.surprise),
        jawOpen=unit(p.jawOpen===undefined?0:p.jawOpen),viewYaw=finite(p.viewYaw===undefined?yaw:p.viewYaw),
        open=Math.max(jawOpen,.68*smile,.45*surprise);
      pose(root,0,0,0,pitch,yaw,0);
      pose(creamFace,0,-1.8*smile,0,0,0,0,1+.04*smile,1,1);
      pose(jaw,0,10,-1,-.38*open,0,0,1+.08*smile-.14*surprise,1,1);
      mouth.visible(open>.015);pose(mouth,0,15+.5*open,22,-.12*open,0,0,1+.25*smile-.45*surprise,.15+.7*open+.4*surprise,1);
      pose(lip,0,14.5-1.1*smile,24,0,0,0,1+.2*smile,1,1);
      eyes[0].update(smile,surprise,viewYaw);eyes[1].update(smile,surprise,viewYaw);
    }
    update({});
    return {root,eyes,nose,jaw,update,eyeClip:true,pointedEar:true,innerEar:true,tongue:false,convexNose:false};
  }
  return {create};
})();
