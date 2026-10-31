// One cloud model; shared authoring and rendering stay in ayatsuri/hone.
'use strict';
globalThis.AyatsuriModels=(()=>{
  const names=['CLOUD'];
  const eye = {color:0x211b24,ambient:.24,diffuse:.6,specular:.7,roughness:.2,
               transmission:0,transmissionColor:0xffffff,thickness:.8};
  const mist = {color:0xeefaff,ambient:.68,diffuse:.32,specular:.03,roughness:.95,
                transmission:.62,transmissionColor:0xe4f5ff,thickness:.5,
                opacity:.93,edgeSoftness:.15};
  function make(scene,index) {
    const parts = [], patterns = [], scratch = {position:[0,0,0],rotation:[0,0,0],scale:[1,1,1]};
    const root = scene.add({name:'actor',geometry:null});
    // place() synchronously copies all components into scene-owned storage.
    // Reuse one complete TRS without retaining a record/arrays for every part.
    function pose(h,x,y,z,roll,sx=1,sy=1,sz=1,yaw=0) {
      const p=scratch.position,r=scratch.rotation,s=scratch.scale;
      p[0]=x;p[1]=y;p[2]=z;r[0]=0;r[1]=yaw;r[2]=roll;s[0]=sx;s[1]=sy;s[2]=sz;h.place(scratch);
    }
    function group(name,parent,position) {
      return scene.add({name,parent,geometry:null,position});
    }
    function lobe(name,parent,position,radii,material,rotation,pattern,detail) {
      const h = scene.add({name,parent,position,rotation:rotation || [0,0,0],scale:[1,1,1],
        geometry:{kind:'ellipsoid',radii,columns:detail ? 8 : 4,rows:detail ? 4 : 2},material,pattern});
      parts.push(h);if(pattern)patterns.push(h);return h;
    }
    let head, halo, cloud;
      const vapor=Object.assign({},mist,{color:0xf5fbff,ambient:.82,diffuse:.38,
        ambientColor:0xc9e1ff,specular:0,transmission:.46,thickness:.44,opacity:.97,edgeSoftness:.15});
      function volume(name,parent,position,lobes,blend) {
        const h=scene.add({name,parent,position,geometry:{kind:'blendEllipsoids',lobes,blend},material:vapor});
        parts.push(h);return h;
      }
      // A broad continuous pear is the primary form; small lobes shape its outline.
      const bodyShape=volume('cloud-body',root,[0,0,0],[
        {position:[0,16,0],radii:[24,29,18]},
        {position:[0,-15,-1],radii:[25,24,17]},
        {position:[-21,-9,-5],radii:[18,20,14]},
        {position:[20,-7,-5],radii:[17,20,14]},
        {position:[-18,22,3],radii:[21,20,18]},
        {position:[19,24,2],radii:[21,22,17]},
        {position:[1,36,-1],radii:[24,15,14]},
        {position:[-7,-29,-8],radii:[20,17,12]}],12);
      head=group('face',root,[0,-12,20]);
      cloud={hands:[],pupils:[],brows:[],cheeks:[],wisps:[],tongueMaterial:{opacity:0,color:0xf29b99}};
      const ink={color:0x30243e,ambient:.6,diffuse:.4,specular:.08,roughness:.85};
      for(let s=-1;s<=1;s+=2) {
        const hand=volume('hand-volume'+s,root,[s*40,20,2],[
          {position:[0,1,0],radii:[9,10,7]},
          {position:[-4,-3,0],radii:[6,7,6]},
          {position:[4,-2,0],radii:[6,7,6]}],6);
        cloud.hands.push(hand);
        // Large ink eyes read against the white body without a second white shell.
        const pupil=lobe('pupil'+s,head,[s*8+s*.4,-2.9,1.5],[4.7,7.3,2],
          Object.assign({},eye,{color:0x292137,specular:.85,roughness:.16}),undefined,undefined,true);
        cloud.pupils.push(pupil);
        cloud.brows.push(lobe('brow'+s,head,[s*8,-14,1],[4,1.1,.8],ink,[0,0,-s*.13]));
        cloud.cheeks.push(lobe('cheek'+s,head,[s*14,5,.8],[4,2.2,.6],
          Object.assign({},mist,{color:0xaecff0,opacity:.55,specular:0})));
      }
      cloud.mouth=lobe('smile',head,[0,10,1.5],[4.2,1.3,.8],Object.assign({},ink,{specular:0}));
      cloud.tongue=lobe('tongue',head,[0,11,2.3],[2.8,.9,.5],
        {color:0xf29b99,ambient:.6,diffuse:.4,opacity:0,specular:0});
      halo=group('iridescent-cloud',null,[0,-61,0]);
      const colors=[0xaacbfa,0xd3c1f3,0xefc1e6,0xb6e5f1,0xeee2bc];
      for(let k=0;k<5;k++) {
        const p={position:[(k-2)*9,(k-2)*(k-2)*.55-1+(k%2)*2,k*.25],rotation:[0,0,(k-2)*.1],scale:[1,1,1]};
        cloud.wisps.push(lobe('halo-wisp'+k,halo,p.position,[14-k*.6,1.7+(k%2)*.7,2.6],
          Object.assign({},mist,{color:colors[k],transmission:.7,thickness:.04,
            opacity:.7,edgeSoftness:.26}),p.rotation));
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
    return {root,parts,patterns,update,name:names[index],index,
      sample:AyatsuriCloudMotion,
      sampleFactory:typeof AyatsuriCloudDynamic==='undefined'?null:AyatsuriCloudDynamic.create};
  }
  return {names,create:make,needsBlend:true};
})();
