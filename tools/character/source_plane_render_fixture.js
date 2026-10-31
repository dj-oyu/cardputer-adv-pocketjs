// Actual native-renderer fixture. Not an app chunk or a device feature.
'use strict';
globalThis.__sourcePlaneFixture=(()=>{
  const H=pocket.kasane.procedural,options={backend:pocket.hone,evaluator:pocket.ayatsuri,geometries:[AyatsuriBlend]};
  const geometry={kind:'blendEllipsoids',blend:8,lobes:[
    {position:[-8,0,0],radii:[20,22,14]},
    {position:[8,0,0],radii:[20,22,14]},
    {position:[0,7,4],radii:[16,18,14]}]};
  const material={color:0xffe8bb,ambient:.3,diffuse:.7,specular:.1,roughness:.8};
  const camera={position:[0,0,300],target:[0,0,0],zoom:1,center:[120,67.5]};
  const light={direction:[-.3,-.4,.8],color:0xffffff};
  const normal=[0,.8,.6];
  let plain=Ayatsuri.scene(options),scene,part,root,held;
  plain.add({name:'body',geometry,material});plain.camera(camera);plain.light(light);
  const resource=H.resource({maxSegments:1024});
  pocket.kasane.replace(tx=>{tx.background(0x000000ff);tx.image({resource,bounds:[0,0,240,135]});});
  function create(){
    scene=AyatsuriLocalPlane.scene(options);root=scene.add({name:'root'});
    part=scene.add({name:'body',parent:root,geometry,material,
      pattern:{kind:'localPlane',normal,offset:3,feather:2,color:0xffe8bb}});
    scene.camera(camera);scene.light(light);
  }
  function draw(s){s.evaluate(0);H.beginFrame(0);s.render();H.commit();}
  function mustThrow(f){try{f();}catch(error){return;}throw Error('expected rejection');}
  function reset(){
    root.place({position:[0,0,0],rotation:[0,0,0],scale:[1,1,1]});
    part.place({position:[0,0,0],rotation:[0,0,0],scale:[1,1,1]});part.visible(true);
    part.material({color:0xe47b30});scene.camera(camera);scene.light(light);
  }
  create();normal.fill(NaN);
  return {
    plain(){draw(plain);},same(){draw(scene);},
    colour(){plain.dispose();plain=null;reset();draw(scene);},
    pose(){root.place({rotation:[.1,-.25,.08],scale:[-1.1,.85,1.2]});part.place({rotation:[-.2,.3,-.1]});draw(scene);},
    light(){scene.light({direction:[.8,-.2,.3]});draw(scene);},
    camera(){scene.camera({position:[-60,25,260],target:[2,1,0],zoom:.9,center:[121,66]});draw(scene);},
    hide(){part.visible(false);draw(scene);},show(){part.visible(true);draw(scene);},
    reset(){reset();draw(scene);},
    snapshot(){
      reset();scene.evaluate(0);H.beginFrame(0);scene.render();
      part.place({rotation:[0,.65,0]});scene.evaluate(1);H.commit();
    },
    invalid(){
      part.place({scale:[1e-20,1,1]});mustThrow(()=>scene.evaluate(2));mustThrow(()=>scene.render());
      reset();draw(scene);
    },
    dispose(){held=part;scene.dispose();scene.dispose();mustThrow(()=>held.visible(true));scene=part=root=null;},
    recreate(){
      normal[0]=0;normal[1]=.8;normal[2]=.6;create();reset();draw(scene);
      mustThrow(()=>held.place({position:[0,0,0]}));held=null;
    },
    cleanup(){if(plain)plain.dispose();if(scene)scene.dispose();plain=scene=part=root=held=null;}
  };
})();
