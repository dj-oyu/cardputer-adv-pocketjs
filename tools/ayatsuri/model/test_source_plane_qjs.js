/* Shared authoring input only. Node exports cold data; live evaluation runs in QuickJS. */
'use strict';
function sourcePlaneDefinitions() {
  return [
    {name:'root', position:[2,-1,3], rotation:[.13,-.21,.07], scale:[-1.1,.8,1.2]},
    {name:'pivot', parent:0, position:[-3,2,1], rotation:[-.09,.17,-.11], scale:[.9,1.2,-.8]},
    {name:'parent-volume', parent:1, position:[-8,0,0], rotation:[.03,-.1,.05], scale:[1.1,.9,1.2],
      geometry:{kind:'blendEllipsoids',blend:4,lobes:[{position:[-5,0,0],radii:[11,9,8]},{position:[6,2,1],radii:[9,11,7]}]},
      material:{color:0xc87838,ambient:.3,diffuse:.7,specular:.1,roughness:.6,opacity:.7,thickness:.4},
      pattern:{kind:'localPlane',normal:[.6,.8,0],offset:2,feather:1.5,color:0xc87838}},
    {name:'child-volume',parent:2,position:[13,4,3],rotation:[-.07,.14,.03],scale:[.8,-1.1,1.3],
      geometry:{kind:'blendEllipsoids',blend:3,lobes:[{position:[-3,0,0],radii:[7,8,6]},{position:[4,1,0],radii:[8,6,7]}]},
      material:{color:0x5080a0,ambient:.4,diffuse:.6,roughness:.7,thickness:.2},
      pattern:{kind:'localPlane',normal:[0,0,-1],offset:-1,feather:0,color:0xd0e080}}
  ];
}
if (typeof nativeCompose === 'function') {
  function check(v,message){if(!v)throw Error(message||'source plane assertion');}
  function throws(f){let caught=false;try{f();}catch(e){caught=true;}check(caught,'expected rejection');}
  const backend={ellipsoidPacked(){throw Error('unexpected ellipse');},blendEllipsoids:nativeBlend};
  const evaluator={composeNode:nativeCompose};
  function scene(){return AyatsuriLocalPlane.scene({backend,evaluator,geometries:[AyatsuriBlend],camera:{position:[0,0,300],center:[120,67.5]},light:{direction:[0,0,1]}});}
  function volume(name){const d=sourcePlaneDefinitions()[2];d.name=name;delete d.parent;return d;}
  globalThis.setupParity=function(){
    globalThis.sourceScene=scene();globalThis.handles=[];
    const defs=sourcePlaneDefinitions();
    for(let i=0;i<defs.length;i++){const d=defs[i];if(d.parent!==undefined)d.parent=handles[d.parent];handles.push(sourceScene.add(d));}
    globalThis.nativeModel=staticModel();globalThis.nativeActor=nativeModel.instance();
    globalThis.camera=new Float32Array(9);globalThis.light=new Float32Array(6);
    globalThis.dirty=new Uint8Array(4);globalThis.pose=new Float32Array(36);globalThis.colors=new Uint32Array(4);globalThis.opacity=new Float64Array(4);
    globalThis.rootPose={position:[0,0,0],rotation:[0,0,0],scale:[-1.1,.8,1.2]};
    globalThis.childPose={rotation:[0,0,0]};
    globalThis.cameraPatch={position:[0,0,300],target:[0,0,0],center:[120,67.5],zoom:1};
    globalThis.lightPatch={direction:[0,0,1],color:0xe0f0ff};
  };
  globalThis.sweep=function(step){
    rootPose.position[0]=2+step*.2;rootPose.position[1]=-1;rootPose.position[2]=3;
    rootPose.rotation[0]=.13+step*.01;rootPose.rotation[1]=-.21+step*.035;rootPose.rotation[2]=.07;
    childPose.rotation[0]=-.07;childPose.rotation[1]=.14-step*.02;childPose.rotation[2]=.03;
    handles[0].place(rootPose);handles[3].place(childPose);
    cameraPatch.position[0]=step*7;cameraPatch.position[1]=step*2;cameraPatch.zoom=.8+step*.025;
    lightPatch.direction[0]=.1+step*.02;lightPatch.direction[1]=-.3;lightPatch.direction[2]=1;
    sourceScene.camera(cameraPatch);sourceScene.light(lightPatch);sourceScene.evaluate(0);clearPackets();sourceScene.render();
    camera.set(cameraPatch.position,0);camera.set(cameraPatch.target,3);camera[6]=cameraPatch.zoom;camera.set(cameraPatch.center,7);
    const len=Math.hypot(...lightPatch.direction);for(let k=0;k<3;k++)light[k]=lightPatch.direction[k]/len;
    light[3]=224/255;light[4]=240/255;light[5]=1;
    dirty.fill(0);dirty[0]=7;dirty[3]=2;pose.set(rootPose.position,0);pose.set(rootPose.rotation,3);pose.set(rootPose.scale,6);pose.set(childPose.rotation,30);
    nativeActor.targets(dirty,pose,colors,opacity);nativeActor.evaluate(camera,light);nativeActor.submit();
  };
  globalThis.warmSource=function(step){if(step){rootPose.position[0]=step%2;handles[0].place(rootPose);}sourceScene.evaluate(0);clearPackets();sourceScene.render();};
  globalThis.closeParity=function(){sourceScene.dispose();nativeActor.dispose();nativeModel.dispose();handles=null;sourceScene=nativeActor=nativeModel=null;};
  globalThis.setupOOM=function(){globalThis.oomScene=scene();globalThis.oldHandle=oomScene.add(volume('old'));oomScene.evaluate(0);clearPackets();oomScene.render();globalThis.addDef=volume('retry');globalThis.added=null;globalThis.sameTurnPhase=0;};
  globalThis.attemptAdd=function(){added=oomScene.add(addDef);};
  globalThis.attemptSameTurn=function(){sameTurnPhase=1;added=oomScene.add(addDef);sameTurnPhase=2;oomScene.evaluate(0);clearPackets();oomScene.render();sameTurnPhase=3;};
  globalThis.setupPlainOOM=function(){
    globalThis.oomScene=Ayatsuri.scene({backend,evaluator,geometries:[AyatsuriBlend],camera:{position:[0,0,300],center:[120,67.5]},light:{direction:[0,0,1]}});
    const old=volume('old');delete old.pattern;globalThis.oldHandle=oomScene.add(old);oomScene.evaluate(0);clearPackets();oomScene.render();
    globalThis.addDef=volume('retry');delete addDef.pattern;globalThis.added=null;
  };
  globalThis.recoverOOM=function(failed){const out={};oomScene.stats(out);check(out.objects===(failed?1:2),'failed add published object');if(failed)added=oomScene.add(addDef);oomScene.evaluate(0);clearPackets();oomScene.render();oomScene.stats(out);check(out.objects===2,'same-name retry');oomScene.dispose();oomScene=oldHandle=added=addDef=null;};
  globalThis.invalidContracts=function(){
    const a=scene(),b=scene(),h=a.add(volume('a'));a.evaluate(0);clearPackets();a.render();const hash=snapshotHash();
    const bad=volume('bad');bad.pattern.normal=[0,0,0];throws(()=>a.add(bad));bad.pattern.normal=[1,0,0];bad.pattern.offset=4097;throws(()=>a.add(bad));
    bad.pattern.offset=0;bad.pattern.feather=1e-5;throws(()=>a.add(bad));bad.pattern.feather=0;bad.geometry.lobes[0].radii[0]=0;throws(()=>a.add(bad));
    const re=volume('reentry');Object.defineProperty(re.pattern,'normal',{get(){a.add(volume('intruder'));return [1,0,0];}});throws(()=>a.add(re));
    const out={};a.stats(out);check(out.objects===1,'invalid/reentrant publish');check(snapshotHash()===hash,'old snapshot changed');
    const foreign=volume('foreign');foreign.parent=h;throws(()=>b.add(foreign));
    const h2=b.add(volume('a'));b.evaluate(0);a.dispose();throws(()=>h.place({position:[1,2,3]}));throws(()=>a.add(volume('x')));
    check(!h._node&&!h._raw&&!h._plane,'retained handle keeps backing state');b.evaluate(0);clearPackets();b.render();h2.visible(false);b.evaluate(0);b.dispose();
    const c=scene();c.add(volume('a'));c.evaluate(0);clearPackets();c.render();c.dispose();
    const plainScene=scene(),plain=volume('plain');delete plain.pattern;plainScene.add(plain);plainScene.evaluate(0);clearPackets();plainScene.render();check(packetCount()===1,'plain five-argument backend');plainScene.dispose();
  };
  globalThis.holdDisposed=function(){globalThis.held=[];const s=scene();for(let i=0;i<8;i++)held.push(s.add(volume('held'+i)));s.evaluate(0);s.dispose();for(const h of held)check(!h._node&&!h._raw&&!h._plane,'retained disposed backing');};
  globalThis.releaseHeld=function(){held=null;};
}
