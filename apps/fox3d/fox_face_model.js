// Expressions over fixed native fox-face nodes. No JS shape/material graph.
'use strict';
globalThis.FoxFaceModel=(()=>{
  // Rounded circumscribing octagon: no single pointed top/bottom vertex.
  const eyeXY=new Float32Array([-1,-.42,-.42,-1,.42,-1,1,-.42,1,.42,.42,1,-.42,1,-1,.42]);
  function finite(v){if(typeof v!=='number'||!Number.isFinite(v))throw new TypeError('FoxFace: finite value');return v;}
  function unit(v){finite(v);if(v<0||v>1)throw new RangeError('FoxFace: expression 0..1');return v;}
  function create(scene){
    if(typeof scene.aperture!=='function'||typeof scene.handle!=='function')throw new TypeError('FoxFace: native clip scene required');
    const h=[];for(let i=0;i<25;i++)h.push(scene.handle(i));
    const root=h[0],jaw=h[4],creamFace=h[6],mouth=h[7],lip=h[23],nose=h[24],parts=[],eyes=[],
      vertices=new Float32Array(24),scratch={position:[0,0,0],rotation:[0,0,0],scale:[1,1,1]};
    for(let i=1;i<25;i++)if(i!==4&&i!==9&&i!==16)parts.push(h[i]);
    function pose(handle,x,y,z,rx,ry,rz,sx=1,sy=1,sz=1){
      const p=scratch.position,r=scratch.rotation,s=scratch.scale;
      p[0]=x;p[1]=y;p[2]=z;r[0]=rx;r[1]=ry;r[2]=rz;s[0]=sx;s[1]=sy;s[2]=sz;handle.place(scratch);
    }
    function segment(handle,ax,ay,bx,by,z){
      const dx=bx-ax,dy=by-ay,length=Math.hypot(dx,dy);
      pose(handle,(ax+bx)*.5,(ay+by)*.5,z,0,0,Math.atan2(dy,dx),length*.52,1,1);
    }
    for(let i=0;i<2;i++){
      const side=i?1:-1,outer=h[2+i];
      scene.aperture(outer,new Float32Array([-.8,.55,.24,side*.12,-.98,.16,.8,.55,.24])).bind(outer);
    }
    function eye(side,id){
      const globe=h[id],color=h[id+1],dark=h[id+2],lidLeft=h[id+3],lidRight=h[id+4],brow=h[id+5];
      for(let i=0;i<8;i++){vertices[i*3]=eyeXY[i*2]*.93;vertices[i*3+1]=eyeXY[i*2+1]*.9;vertices[i*3+2]=.7;}
      const aperture=scene.aperture(globe,vertices);aperture.bind(globe).bind(color).bind(dark);
      return {aperture,update(smile,surprise){
        const closed=smile>=.98,height=Math.max(.03,.9*(1-smile)+.2*surprise),width=.93+.07*surprise;
        for(let i=0;i<8;i++){
          vertices[i*3]=eyeXY[i*2]*width;vertices[i*3+1]=eyeXY[i*2+1]*height-.04*smile;vertices[i*3+2]=.7;
        }
        aperture.setVertices(vertices);aperture.setEmpty(closed?'closed':null);
        // Open eyes use their curved sphere boundary, not an ink roof. Restore
        // the original full-smile arc exactly, including its F32 endpoint.
        const arc=Math.max(0,Math.min(1,(smile-.7)/.28)),endpoint=Math.fround(Math.fround(.9)*(1+.04*surprise))*8.4,
          corner=closed?.4:(1-arc)*(-.4*smile)+arc*.4,
          peak=closed?-2.8:(1-arc)*(-.88*height*10-.4*smile)-arc*2.8,z=vertices[2]*3;
        lidLeft.visible(smile>.7);lidRight.visible(smile>.7);
        segment(lidLeft,-endpoint,corner,0,peak,z);segment(lidRight,0,peak,endpoint,corner,z);
        pose(brow,0,-13-4*surprise+1.5*smile,0,0,0,-side*(.08+.12*smile-.12*surprise));
      }};
    }
    eyes.push(eye(-1,10));eyes.push(eye(1,17));
    function update(p){
      const yaw=finite(p.yaw===undefined?0:p.yaw),pitch=finite(p.pitch===undefined?0:p.pitch),
        smile=unit(p.smile===undefined?0:p.smile),surprise=unit(p.surprise===undefined?0:p.surprise),
        jawOpen=unit(p.jawOpen===undefined?0:p.jawOpen),open=Math.max(jawOpen,.68*smile,.45*surprise);
      pose(root,0,0,0,pitch,yaw,0);
      pose(creamFace,0,-1.8*smile,0,0,0,0,1+.04*smile,1,1);
      pose(jaw,0,10,-1,-.38*open,0,0,1+.08*smile-.14*surprise,1,1);
      mouth.visible(open>.015);pose(mouth,0,15+1.5*open,24,0,0,0,1+.25*smile-.45*surprise,.15+.9*open+.5*surprise,1);
      pose(lip,0,14.5-1.1*smile,24,0,0,0,1+.2*smile,1,1);
      eyes[0].update(smile,surprise);eyes[1].update(smile,surprise);
    }
    update({});
    return {root,parts,eyes,nose,jaw,update,eyeClip:true,pointedEar:true,innerEar:false,tongue:false,convexNose:false};
  }
  return {create};
})();
