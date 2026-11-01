// Experimental adapter generated from 7d21b3020b649aadc343c367f60948eca4abaf62; immutable definitions remain native.
'use strict';
globalThis.AyatsuriCloudUpdate={create(scene,index){
  const h=[];for(let i=0;i<19;i++)h.push(scene.handle(i));
  const root=h[0],bodyShape=h[1],head=h[2],halo=h[13],parts=[h[1],h[3],h[4],h[5],h[6],h[7],h[8],h[9],h[10],h[11],h[12],h[14],h[15],h[16],h[17],h[18]],patterns=[],
    scratch={position:[0,0,0],rotation:[0,0,0],scale:[1,1,1]},
    cloud={hands:[h[3],h[7]],pupils:[h[4],h[8]],brows:[h[5],h[9]],cheeks:[h[6],h[10]],wisps:[h[14],h[15],h[16],h[17],h[18]],mouth:h[11],tongue:h[12],tongueMaterial:{opacity:0,color:0xf29b99}};
    function pose(h,x,y,z,roll,sx=1,sy=1,sz=1,yaw=0) {
      const p=scratch.position,r=scratch.rotation,s=scratch.scale;
      p[0]=x;p[1]=y;p[2]=z;r[0]=0;r[1]=yaw;r[2]=roll;s[0]=sx;s[1]=sy;s[2]=sz;h.place(scratch);
    }
    function update(timeMs,body,expression) {
      // Inputs are explicit so body-only scripts do not smuggle in breathing.
      const size=.9;
      // Actor motion is rigid; only the body volume receives squash/stretch.
      // Facial proportions and detached hands must not inherit a rubber scale.
      pose(bodyShape,0,0,0,0,1/body.stretch,body.stretch);
      pose(root,(body.shiftX||0)*size,body.lift*size+4,0,body.tilt,size,size,size,body.turn);
      pose(head,0,-12*body.stretch,20,body.head);
      const smile=expression.smile||0,surprise=expression.surprise||0,brow=expression.brow||0,
        open=Math.max(.04,expression.open*(1-expression.blink)*(1-.28*smile+.12*surprise));
      if(cloud) {
        for(let k=0;k<2;k++) {
          const s=k?1:-1,gesture=(k?body.handRight:body.handLeft)||0;
          pose(cloud.hands[k],s*(40-gesture*8),20-gesture*22,2,s*gesture);
          // Match the former child's F32 local position and parent F32 scale
          // before folding their identity rotation into the pupil transform.
          const x=Math.fround(s*.4+(expression.gazeX||0)*1.1),
            y=Math.fround(.1+(expression.gazeY||0)*1.2),h=Math.fround(open);
          pose(cloud.pupils[k],s*8+x,-3+h*y,1.5,0,1,h);
          pose(cloud.brows[k],s*8,-14-brow*2-3.5*surprise-1.2*smile,1,s*(.1+.08*smile-.05*surprise));
          pose(cloud.cheeks[k],s*14,5-2.8*smile+.7*surprise,.8,s*.12*smile);
        }
        // Reuse the two mouth surfaces: joined upturned strokes for joy,
        // a tall dark opening with a pink interior for surprise. No new parts.
        pose(cloud.mouth,-2.4*smile,10,1.5,.35*smile,1+.2*smile-.13*surprise,1-.25*smile+4*surprise);
        pose(cloud.tongue,2.4*smile,11-.8*smile+1.4*surprise,2.3,-.35*smile,1+.8*smile-.2*surprise,1+.15*smile+1.7*surprise);
        const inkMix=smile+surprise>0?smile/(smile+surprise):0;
        cloud.tongueMaterial.color=(Math.round(242-194*inkMix)<<16)|(Math.round(155-119*inkMix)<<8)|Math.round(153-91*inkMix);
        cloud.tongueMaterial.opacity=Math.min(1,(smile+surprise)*2);cloud.tongue.material(cloud.tongueMaterial);
      }
      if(halo) {
        pose(halo,body.haloOffset+(body.shiftX||0)*.3,-61+body.lift*.2,0,body.tilt*.12+(body.haloFollow||0),1+.08*Math.abs(body.tilt));
        if(cloud)for(let k=0;k<cloud.wisps.length;k++) {
          const side=k-2,lag=body.haloFollow||0;
          const center=body.wispCenter||0,tip=(side<0?body.wispLeft:body.wispRight)||0,
            bend=center+(tip-center)*side*side/4;
          const spreadCenter=body.wispSpreadCenter||0,
            spreadTip=(side<0?body.wispSpreadLeft:body.wispSpreadRight)||0,
            spread=spreadCenter+(spreadTip-spreadCenter)*side*side/4;
          pose(cloud.wisps[k],side*9*(1+.12*spread),side*side*.55-1+(k%2)*2+bend,k*.25,
            side*.1+(tip-center)*side/18,1+Math.abs(lag)*(.4+Math.abs(side)*.15)+.2*spread);
        }
      }
    }
  return {root,parts,patterns,update,name:'CLOUD',index,sample:AyatsuriCloudMotion,sampleFactory:typeof AyatsuriCloudDynamic==='undefined'?null:AyatsuriCloudDynamic.create};
}};
