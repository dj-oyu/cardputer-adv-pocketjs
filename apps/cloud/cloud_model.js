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
    const parts = [], patterns = [], pose = {position:[0,0,0],rotation:[0,0,0],scale:[1,1,1]};
    const root = scene.add({name:'actor',geometry:null,position:pose.position,rotation:pose.rotation,scale:pose.scale});
    function group(name,parent,position) {
      return scene.add({name,parent,geometry:null,position,rotation:[0,0,0],scale:[1,1,1]});
    }
    function lobe(name,parent,position,radii,material,rotation,pattern,detail) {
      const h = scene.add({name,parent,position,rotation:rotation || [0,0,0],scale:[1,1,1],
        geometry:{kind:'ellipsoid',radii,columns:detail ? 8 : 4,rows:detail ? 4 : 2},material,pattern});
      parts.push(h);if(pattern)patterns.push(h);return h;
    }
    let head, halo, leftEye, rightEye, cloud;
    const bodyPose={scale:[1,1,1]};
    const headPose = {rotation:[0,0,0]},
      haloPose = {position:[0,-38,0],rotation:[0,0,0],scale:[1,1,1]},
      leftEyePose = {scale:[1,1,1]}, rightEyePose = {scale:[1,1,1]};
      const vapor=Object.assign({},mist,{color:0xf5fbff,ambient:.8,diffuse:.55,
        ambientColor:0xaecff4,specular:.012,transmission:.46,thickness:.44,opacity:.97,edgeSoftness:.09});
      function volume(name,parent,position,lobes,blend) {
        const h=scene.add({name,parent,position,geometry:{kind:'blendEllipsoids',lobes,blend},material:vapor});
        parts.push(h);return h;
      }
      // A broad continuous pear is the primary form; small lobes shape its outline.
      const bodyShape=volume('cloud-body',root,[0,0,0],[
        {position:[0,16,0],radii:[29,30,18]},
        {position:[0,-15,-1],radii:[25,24,17]},
        {position:[-21,-9,-5],radii:[18,20,14]},
        {position:[20,-7,-5],radii:[17,20,14]},
        {position:[-20,22,-4],radii:[20,20,14]},
        {position:[20,24,-4],radii:[21,22,14]},
        {position:[1,36,-3],radii:[24,15,14]},
        {position:[-7,-29,-8],radii:[20,17,12]}],12);
      head=group('face',root,[0,-12,20]);
      headPose.position=[0,-12,20];
      cloud={hands:[],handPoses:[],pupils:[],gaze:[],brows:[],browPoses:[],wisps:[],wispPoses:[],
        mouthPose:{scale:[1,1,1]},tongueMaterial:{opacity:0}};
      const ink={color:0x30243e,ambient:.6,diffuse:.4,specular:.08,roughness:.85};
      for(let s=-1;s<=1;s+=2) {
        const hand=group('floating-hand'+s,root,[s*40,20,2]);
        cloud.hands.push(hand);cloud.handPoses.push({position:[s*40,20,2],rotation:[0,0,0]});
        volume('hand-volume'+s,hand,[0,0,0],[
          {position:[0,1,0],radii:[9,10,7]},
          {position:[-4,-3,0],radii:[6,7,6]},
          {position:[4,-2,0],radii:[6,7,6]}],6);
        const eyeGroup=group('eye'+s,head,[s*8,-3,0]);
        // Large ink eyes read against the white body without a second white shell.
        const pupil=lobe('pupil'+s,eyeGroup,[s*.4,.1,1.5],[4.7,7.3,2],
          Object.assign({},eye,{color:0x292137,specular:.85,roughness:.16}),undefined,undefined,true);
        cloud.pupils.push(pupil);cloud.gaze.push({position:[s*.4,.1,1.5]});
        if(s<0)leftEye=eyeGroup;else rightEye=eyeGroup;
        cloud.brows.push(lobe('brow'+s,head,[s*8,-14,1],[4,1.1,.8],ink,[0,0,-s*.13]));
        cloud.browPoses.push({position:[s*8,-14,1],rotation:[0,0,-s*.13]});
        lobe('cheek'+s,head,[s*14,5,.8],[4,2.2,.6],
          Object.assign({},mist,{color:0xaecff0,opacity:.55,specular:0}));
      }
      cloud.mouth=lobe('smile',head,[0,10,1.5],[4.2,1.3,.8],ink);
      cloud.tongue=lobe('tongue',head,[0,11,2.3],[2.8,.9,.5],
        {color:0xf29b99,ambient:.8,diffuse:.2,opacity:0,specular:0});
      haloPose.position[1]=-61;
      halo=group('iridescent-cloud',null,haloPose.position);
      const colors=[0xaacbfa,0xd3c1f3,0xefc1e6,0xb6e5f1,0xeee2bc];
      for(let k=0;k<5;k++) {
        const p={position:[(k-2)*9,(k-2)*(k-2)*.55-1,k*.25],rotation:[0,0,(k-2)*.1],scale:[1,1,1]};
        cloud.wispPoses.push(p);
        cloud.wisps.push(lobe('halo-wisp'+k,halo,p.position,[14-k*.6,1.5+(k%2)*.6,2.6],
          Object.assign({},mist,{color:colors[k],transmission:.7,thickness:.04,
            opacity:.7,edgeSoftness:.26}),p.rotation));
      }
    function update(timeMs,body,expression) {
      // Inputs are explicit so body-only scripts do not smuggle in breathing.
      pose.rotation[2]=body.tilt;pose.rotation[1]=body.turn;
      const size=.9;
      // Actor motion is rigid; only the body volume receives squash/stretch.
      // Facial proportions and detached hands must not inherit a rubber scale.
      pose.scale[0]=pose.scale[1]=pose.scale[2]=size;
      bodyPose.scale[0]=1/body.stretch;bodyPose.scale[1]=body.stretch;bodyShape.place(bodyPose);
      pose.position[0]=(body.shiftX||0)*size;pose.position[1]=body.lift*size+(cloud?4:0);root.place(pose);
      headPose.position[1]=-12*body.stretch;
      headPose.rotation[2]=body.head;head.place(headPose);
      const open=Math.max(.04,expression.open*(1-expression.blink));
      leftEyePose.scale[1]=open;rightEyePose.scale[1]=open;
      leftEye.place(leftEyePose);rightEye.place(rightEyePose);
      if(cloud) {
        const smile=expression.smile||0,brow=expression.brow||0;
        for(let k=0;k<2;k++) {
          const s=k?1:-1,h=cloud.handPoses[k],gesture=(k?body.handRight:body.handLeft)||0;
          h.position[0]=s*(40-gesture*8);h.position[1]=20-gesture*22;
          h.rotation[2]=s*gesture;cloud.hands[k].place(h);
          cloud.gaze[k].position[0]=s*.4+(expression.gazeX||0)*1.1;
          cloud.gaze[k].position[1]=.1+(expression.gazeY||0)*1.2;cloud.pupils[k].place(cloud.gaze[k]);
          const b=cloud.browPoses[k];b.position[1]=-14-brow*3;b.rotation[2]=-s*(.13+brow*.3);
          cloud.brows[k].place(b);
        }
        cloud.mouthPose.scale[0]=1+smile*.9;cloud.mouthPose.scale[1]=1+smile*3;cloud.mouth.place(cloud.mouthPose);
        cloud.tongueMaterial.opacity=Math.min(1,smile*2);cloud.tongue.material(cloud.tongueMaterial);
      }
      if(halo) {
        haloPose.position[0]=body.haloOffset+(body.shiftX||0)*.3;
        haloPose.position[1]=-61+body.lift*.2;haloPose.rotation[2]=body.tilt*.12+(body.haloFollow||0);
        haloPose.scale[0]=1+.08*Math.abs(body.tilt);halo.place(haloPose);
        if(cloud)for(let k=0;k<cloud.wisps.length;k++) {
          const p=cloud.wispPoses[k],side=k-2,lag=body.haloFollow||0;
          const center=body.wispCenter||0,tip=(side<0?body.wispLeft:body.wispRight)||0,
            bend=center+(tip-center)*Math.abs(side)/2;
          p.position[1]=side*side*.55-1+bend;
          p.rotation[2]=side*.1+(side<0?center-tip:tip-center)/18;
          p.scale[0]=1+Math.abs(lag)*(.4+Math.abs(side)*.15);
          cloud.wisps[k].place(p);
        }
      }
    }
    return {root,parts,patterns,update,name:names[index],index,sample:AyatsuriCloudMotion};
  }
  return {names,create:make,needsBlend:true};
})();
