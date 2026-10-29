// Models own visual semantics; light and camera stay in the app's scene.
'use strict';
globalThis.CharacterModels = (() => {
  const names = ['HUMAN', 'FOX', 'CLOUD'];
  const skin = {color:0xe9a87f,ambient:.38,diffuse:.62,specular:.12,roughness:.7,
                transmission:.14,transmissionColor:0xffad79,thickness:.65};
  const hair = {color:0x64331f,ambient:.32,diffuse:.68,specular:.44,roughness:.38,
                transmission:.03,transmissionColor:0xc87947,thickness:.8};
  const cloth = {color:0x257e84,ambient:.38,diffuse:.62,specular:.02,roughness:.95,
                 transmission:0,transmissionColor:0xffffff,thickness:.8};
  const cream = {color:0xf3dfb2,ambient:.42,diffuse:.58,specular:.02,roughness:.9,
                 transmission:.05,transmissionColor:0xffdfae,thickness:.8};
  const fur = {color:0xd76b25,ambient:.4,diffuse:.6,specular:.035,roughness:.88,
               transmission:.1,transmissionColor:0xffc782,thickness:.9};
  const eye = {color:0x211b24,ambient:.24,diffuse:.6,specular:.7,roughness:.2,
               transmission:0,transmissionColor:0xffffff,thickness:.8};
  const mist = {color:0xeefaff,ambient:.68,diffuse:.32,specular:.03,roughness:.95,
                transmission:.62,transmissionColor:0xe4f5ff,thickness:.5,
                opacity:.93,edgeSoftness:.15};
  function recolor(material,color) { return Object.assign({},material,{color}); }
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
    let head, tail, halo, leftEye, rightEye;
    const headPose = {rotation:[0,0,0]}, tailPose = {rotation:[0,0,0]},
      haloPose = {position:[0,-38,0],rotation:[0,0,0],scale:[1,1,1]},
      leftEyePose = {scale:[1,1,1]}, rightEyePose = {scale:[1,1,1]};
    if (index === 0) {
      head = group('head',root,[0,-25,0]);
      lobe('bob-back',head,[0,0,-4],[17,16,8],hair,undefined,undefined,true);
      lobe('shirt',root,[0,4,0],[10,14,5],cream);
      lobe('jacket-left',root,[-8,3,3],[4,14,4],cloth,[0,0,.06]);
      lobe('jacket-right',root,[8,3,3],[4,14,4],cloth,[0,0,-.06]);
      for (let s=-1;s<=1;s+=2) {
        lobe('sleeve'+s,root,[s*13,3,0],[4,10,4],cloth,[0,0,-s*.19]);
        lobe('hand'+s,root,[s*17,16,1],[3,7,3],skin,[0,0,-s*.14]);
        lobe('shorts'+s,root,[s*5,20,1],[6,7,5],recolor(cloth,0xd16d51));
        lobe('leg'+s,root,[s*5,31,0],[3.5,9,3.5],skin);
        lobe('boot'+s,root,[s*5,43,2],[5,6,5],recolor(cloth,0xb98832));
        lobe('ear'+s,head,[s*13,1,2],[3,4,2],Object.assign({},skin,{transmission:.4,thickness:.16}));
      }
      lobe('face',head,[0,1,4],[13,14,9],skin,undefined,undefined,true);
      for (let s=-1;s<=1;s+=2) {
        lobe('bob-side'+s,head,[s*13,3,3],[5,12,5],hair,[0,0,-s*.27]);
        lobe('fringe'+s,head,[s*6,-10,9],[8,5,4],hair,[0,0,s*.35]);
        lobe('warm-cheek'+s,head,[s*8,4,11.5],[2.8,1.5,.6],
          Object.assign({},skin,{color:0xe9a18c,specular:.02,roughness:.95,transmission:.22}));
        const h=lobe('eye'+s,head,[s*5,-1,12.5],[2.4,3.5,1.3],eye,undefined,undefined,true);
        if(s<0)leftEye=h;else rightEye=h;
      }
      lobe('nose',head,[0,3,13],[1.4,1.2,2],Object.assign({},skin,{specular:.23,roughness:.5}));
      lobe('forehead',head,[0,-5,12],[2.8,1.5,.5],Object.assign({},skin,{specular:.19,roughness:.6}));
      lobe('mouth',head,[0,7,12],[2.8,.6,.8],recolor(skin,0x984e47));
    } else if (index === 1) {
      tail=group('tail',root,[20,9,-3]);
      lobe('tail-root',tail,[5,-1,0],[12,12,7],fur,[0,0,-.4]);
      lobe('tail-arc',tail,[14,-13,0],[11,18,8],fur,[0,0,.4],
        {kind:'band',axis:'v',start:0,end:.36,color:0xf5dfb6},true);
      lobe('tail-tip',tail,[11,-27,2],[11,9,7],cream,[0,0,-.3]);
      lobe('body',root,[6,13,-2],[23,11,8],fur,undefined,undefined,true);
      for (let s=-1;s<=1;s+=2) for (let k=0;k<2;k++) {
        const x=k ? 20 : -8;
        lobe('leg'+s+k,root,[x,27,s*4],[3.5,9,3.5],fur);
        lobe('paw'+s+k,root,[x-1,34,s*4+1],[4.5,3,4],recolor(fur,0x493124));
      }
      head=group('head',root,[-15,-6,3]);
      for (let s=-1;s<=1;s+=2) {
        lobe('ear'+s,head,[s*10,-15,-1],[5.5,12,3],fur,[0,0,s*.25]);
        lobe('inner-ear'+s,head,[s*10,-16,2],[3,8,1.5],
          {color:0xe8ad82,ambient:.4,diffuse:.6,specular:.02,roughness:.9,
           transmission:.5,transmissionColor:0xffb873,thickness:.12},[0,0,s*.25]);
      }
      lobe('cheeks',head,[0,2,0],[15,12,8],fur,undefined,undefined,true);
      lobe('chest',root,[-11,13,5],[8,11,4],cream,[0,0,-.1]);
      for (let s=-1;s<=1;s+=2) {
        lobe('muzzle'+s,head,[s*5,7,8],[7,4,4],cream,undefined,undefined,true);
        const h=lobe('eye'+s,head,[s*6,-1,7.6],[2.6,3.5,1.8],eye,undefined,undefined,true);
        if(s<0)leftEye=h;else rightEye=h;
      }
      lobe('nose',head,[0,5,12],[2.4,1.8,2],eye,undefined,undefined,true);
    } else {
      head=group('face',root,[0,0,0]);
      lobe('body-volume',head,[0,14,-2],[23,24,12],mist,undefined,undefined,true);
      const lobes=[[-17,-4,0,13,15,9],[1,-13,-1,16,17,10],[16,-3,0,13,16,9],
        [-19,20,0,12,13,9],[17,23,0,12,13,9],[-8,32,2,14,11,8],[8,33,1,13,11,9]];
      for(let k=0;k<lobes.length;k++) {
        const b=lobes[k];
        lobe('cloud-lobe'+k,head,[b[0],b[1],b[2]],[b[3],b[4],b[5]],
          Object.assign({},mist,{thickness:.3+.06*k}),undefined,undefined,k===1);
      }
      for(let s=-1;s<=1;s+=2) {
        lobe('floating-hand'+s,root,[s*35,15,3],[8,9,7],mist);
        const h=lobe('eye'+s,head,[s*7,0,12],[2.7,4.4,1.8],eye,undefined,undefined,true);
        if(s<0)leftEye=h;else rightEye=h;
        lobe('cheek'+s,head,[s*12,6,10],[3.3,1.6,1],recolor(mist,0xa8ceeb));
      }
      lobe('smile',head,[0,9,12],[3,.7,1],recolor(eye,0x654766));
      halo=group('iridescent-cloud',root,haloPose.position);
      const colors=[0xcbdcf7,0xd8ebed,0xe9d9eb,0xe5e5ba];
      for(let k=0;k<4;k++)lobe('halo-wisp'+k,halo,[(k-1.5)*11,Math.sin(k)*1.3,k*.2],
        [17,1.4,3],Object.assign({},mist,{color:colors[k],transmission:.7,thickness:.06,
          opacity:.6,edgeSoftness:.3}),[0,0,-.03]);
    }
    function update(timeMs,body,expression) {
      // Inputs are explicit so body-only scripts do not smuggle in breathing.
      pose.rotation[2]=body.tilt;pose.rotation[1]=body.turn;
      pose.scale[0]=1/body.stretch;pose.scale[1]=body.stretch;pose.scale[2]=1;
      pose.position[1]=body.lift;root.place(pose);
      headPose.rotation[2]=body.head;head.place(headPose);
      const open=Math.max(.04,expression.open*(1-expression.blink));
      leftEyePose.scale[1]=open;rightEyePose.scale[1]=open;
      leftEye.place(leftEyePose);rightEye.place(rightEyePose);
      if(tail) {tailPose.rotation[2]=body.tail;tail.place(tailPose);}
      if(halo) {
        haloPose.position[0]=body.haloOffset;haloPose.rotation[2]=-body.tilt*.5;
        haloPose.scale[0]=1+.08*Math.abs(body.tilt);halo.place(haloPose);
      }
    }
    return {root,parts,patterns,update,name:names[index],index};
  }
  return {names,create:make};
})();
