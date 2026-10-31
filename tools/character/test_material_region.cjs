'use strict';
const assert=require('node:assert/strict');
const Ayatsuri=require('../../apps/character/character.js');
const LocalPlane=require('../../apps/lib/ayatsuri_local_plane.js');
const TransformReference=require('./transform_reference.cjs');
const plain=Ayatsuri.scene({backend:{ellipsoid(){}},evaluator:TransformReference});
assert.throws(()=>plain.add({name:'requires-region-factory',geometry:{kind:'ellipsoid',radii:[1,1,1]},
  pattern:{kind:'localPlane',normal:[0,1,0],color:0xffffff}}),/pattern kind/);
plain.dispose();
const scene=LocalPlane.scene({backend:{ellipsoid(){}},evaluator:TransformReference});
const normal=[0,2,0],body=scene.add({name:'body',geometry:{kind:'ellipsoid',radii:[20,30,10]},
  material:{color:0xe47b30,specular:.5},pattern:{kind:'localPlane',normal,offset:.2,feather:.1,color:0xffe8bb}});
normal.fill(NaN);
delete globalThis.AyatsuriLocalPlane;
function packet(){let value;scene.visit(p=>{value=p;});return value;}
function stats(){const s={};scene.stats(s);return s;}
scene.evaluate(0);
const p=packet(),texture=Array.from(p.texture),projection=Array.from(p.screen),normals=Array.from(p.normalMatrix),before=stats();
assert.equal(p.surface[4],4);assert.deepEqual(texture.slice(0,6),[0,1,0,Math.fround(.2),Math.fround(.1),0]);
assert.deepEqual(texture.slice(7),[0,1]);
scene.light({direction:[.8,.1,-.5]});scene.evaluate(5);
assert.deepEqual(Array.from(packet().texture),texture,'light does not move body markings');
assert.deepEqual(Array.from(packet().screen),projection);assert.deepEqual(Array.from(packet().normalMatrix),normals);
assert.equal(stats().projectionUpdates,before.projectionUpdates);
body.place({rotation:[.2,.4,-.1],scale:[1.3,.7,1.1]});scene.evaluate(10);
assert.deepEqual(Array.from(packet().texture),texture,'local region follows transformed surface');
assert.notDeepEqual(Array.from(packet().screen),projection);
globalThis.AyatsuriLocalPlane=LocalPlane;
assert.throws(()=>body.texture({offset:[.1,0]}),/localPlane/);
const beforeInvalid=stats();
const invalid=[
  [{normal:[0,0,0]},/localPlane range/], [{normal:[0,NaN,0]},/finite number required/],
  [{normal:[0,1,0],offset:2.1},/localPlane range/], [{normal:[0,1,0],feather:1e-5},/localPlane range/],
  [{normal:[0,1,0],feather:Infinity},/finite number required/], [{normal:[0,1,0],color:0x1000000},/RGB24 required/]
];
for(let index=0;index<invalid.length;++index){
  const [pattern,reason]=invalid[index];
  assert.throws(()=>scene.add({name:'invalid-plane-'+index,geometry:{kind:'ellipsoid',radii:[1,1,1]},
    pattern:{kind:'localPlane',color:0xffffff,...pattern}}),reason);
  assert.deepEqual(stats(),beforeInvalid,'failed construction does not publish a node');
}
scene.render();
scene.add({name:'valid-plane-control',geometry:{kind:'ellipsoid',radii:[1,1,1]},
  pattern:{kind:'localPlane',normal:[0,1,0],offset:0,feather:.1,color:0xffffff}});
scene.evaluate(15);scene.render();
assert.throws(()=>scene.add({name:'reentrant',geometry:{kind:'ellipsoid',radii:[1,1,1]},
  get pattern(){body.place({position:[1,0,0]});return null;}}),/busy scene/);
scene.render();
body.place({position:[1,0,0]});
assert.throws(()=>scene.render(),/evaluate before render/);
scene.evaluate(20);scene.render();
assert.throws(()=>scene.light({get direction(){body.place({position:[99,0,0]});return [0,0,1];}}),/busy scene/);
assert.throws(()=>scene.stats({set objects(value){body.visible(false);}}),/busy scene/);
assert.throws(()=>body.material({get color(){body.visible(false);return 0xffffff;}}),/busy scene/);
scene.render();
scene.dispose();
scene.dispose();
assert.throws(()=>body.texture({offset:[0,0]}),/disposed scene/);
assert.throws(()=>scene.evaluate(25),/disposed scene/);
// Synthetic allocation faults cover both adapter and base handle creation.
// These are V8 fault injections, not proof of a real QuickJS allocator OOM.
for(let fault=1;fault<=3;fault++){
  const atomic=LocalPlane.scene({backend:{ellipsoid(){}},evaluator:TransformReference});
  const def={name:'retry-after-allocation-failure',geometry:{kind:'ellipsoid',radii:[1,1,1]},
    pattern:{kind:'localPlane',normal:[0,1,0],color:0xffffff}};
  const original=Object.create;let calls=0;
  Object.create=function(proto){if(++calls===fault)throw new Error('synthetic allocation failure');return original(proto);};
  try{assert.throws(()=>atomic.add(def),/synthetic allocation failure/);}finally{Object.create=original;}
  const state={};atomic.stats(state);assert.equal(state.objects,0);
  atomic.add(def);atomic.evaluate(0);atomic.render();atomic.dispose();
}
{
  const atomic=LocalPlane.scene({backend:{ellipsoid(){}},evaluator:TransformReference});
  const def={name:'retry-after-record-reservation',geometry:{kind:'ellipsoid',radii:[1,1,1]},
    pattern:{kind:'localPlane',normal:[0,1,0],color:0xffffff}};
  const original=Array.prototype.push;
  Array.prototype.push=function(value){
    if(value&&value.pattern&&value.work===null)throw new Error('synthetic record reservation failure');
    return original.apply(this,arguments);
  };
  try{assert.throws(()=>atomic.add(def),/synthetic record reservation failure/);}finally{Array.prototype.push=original;}
  const state={};atomic.stats(state);assert.equal(state.objects,0);
  atomic.add(def);atomic.evaluate(0);atomic.render();atomic.dispose();
}
// Blend planes are evaluated once in the common node frame, not each lobe q.
const Blend=require('../../apps/character/character_blend.js');
const blendGeometry={kind:'blendEllipsoids',blend:2,lobes:[
  {position:[-2,0,1],radii:[4,3,2]},{position:[3,1,-1],radii:[3,4,2]}]};
const camera={position:[37,-21,400],target:[0,0,0],zoom:.83,center:[120,75]};
function blended(region=true){
  const calls=[],backend={tag:17,ellipsoid(){},blendEllipsoids(){assert.equal(this.tag,17);calls.push(Array.from(arguments));}};
  const scene=LocalPlane.scene({backend,evaluator:TransformReference,geometries:[Blend],camera});
  const parent=scene.add({name:'pivot',geometry:null,position:[3,1,-2],rotation:[.15,-.25,.1],scale:[-1.2,.8,1.1]});
  const normal=[.6,.8,0],def={name:'blend',parent,position:[1,2,3],rotation:[.1,.3,-.2],scale:[.9,-1.1,1.3],geometry:blendGeometry,material:{color:0xe47b30}};
  if(region)def.pattern={kind:'localPlane',normal,offset:.4,feather:.2,color:0xe47b30};
  const body=scene.add(def);normal.fill(NaN);return {scene,parent,body,calls,backend};
}
function blendPacket(scene){let p;scene.visit((value,name)=>{if(name==='blend')p=value;});return p;}
function classified(p){
  const n=p.localPlaneNormal,rx=p.local[3],ry=p.local[4],rz=p.local[5],a=p.lobes;
  for(const point of [[0,0,0],[-3,2,7],[4,-1,-2]]){
    const qx=(point[0]-p.local[0])/rx,qy=(point[1]-p.local[1])/ry,qz=(point[2]-p.local[2])/rz,
      sx=a[0]*qx+a[1]*qy+a[2]*qz+a[3],sy=a[4]*qx+a[5]*qy+a[6]*qz+a[7],sz=a[17]*qx+a[18]*qy+a[19]*qz+a[20],
      actual=p.localPlane[0]*sx+p.localPlane[1]*sy+p.localPlane[2]*sz+p.localPlane[3],
      expected=Math.fround(n[0])*point[0]+Math.fround(n[1])*point[1]+Math.fround(n[2])*point[2]-p.localPlaneOffset;
    assert.ok(Math.abs(actual-expected)<1e-4,`common-local classification ${actual} != ${expected}`);
  }
}
const masked=blended(),unmasked=blended(false);
masked.scene.evaluate(0);unmasked.scene.evaluate(0);masked.scene.render();unmasked.scene.render();
let bp=blendPacket(masked.scene),up=blendPacket(unmasked.scene);
assert.equal(masked.calls[0].length,7);assert.equal(unmasked.calls[0].length,5);
assert.equal(bp.localPlane.buffer,masked.body._node.matrix.buffer);assert.equal(bp.localPlaneColor.buffer,bp.localPlane.buffer);
assert.equal(bp.localPlane.byteOffset,316+108*2);assert.equal(bp.localPlaneColor.byteOffset,316+108*2+20);
assert.equal(bp.localPlane.buffer.byteLength,316+108*2+24);assert.equal(unmasked.body._node.matrix.buffer.byteLength,316+108*2);
assert.equal(bp.localPlane.length,5);assert.equal(bp.localPlaneColor.length,1);assert.equal(bp.patternMode,5);
assert.deepEqual(Array.from(bp.lobes),Array.from(up.lobes),'markings do not modify lobe geometry/normals');
assert.equal(bp.localPlaneColor[0],bp.surface[0],'same A/B albedo is supported');classified(bp);
const ownedPlane=bp.localPlane,ownedColor=bp.localPlaneColor,firstPlane=Array.from(ownedPlane),st={};masked.scene.stats(st);const updates=st.localPlaneUpdates;
masked.scene.evaluate(0);masked.scene.light({direction:[.2,.7,-.5]});masked.scene.evaluate(1);
masked.scene.stats(st);assert.equal(st.localPlaneUpdates,updates);assert.deepEqual(Array.from(ownedPlane),firstPlane);
masked.parent.place({rotation:[-.2,.1,.3],scale:[-1.4,.7,1.8]});masked.scene.evaluate(2);classified(blendPacket(masked.scene));
masked.scene.camera({position:[-90,60,300],zoom:.97,center:[117,73]});masked.scene.evaluate(3);classified(blendPacket(masked.scene));
assert.equal(bp.localPlane,ownedPlane);assert.equal(bp.localPlaneColor,ownedColor);
const savedB=ownedColor[0];masked.body.material({color:0x123456});masked.scene.evaluate(4);assert.equal(ownedColor[0],savedB);
assert.throws(()=>masked.scene.add({name:'bad-blend',geometry:blendGeometry,pattern:{kind:'localPlane',normal:[0,1,0],offset:4096.1,color:0}}),/localPlane range/);
assert.throws(()=>masked.scene.add({name:'bad-blend',geometry:blendGeometry,pattern:{kind:'localPlane',normal:[0,1,0],feather:9e-5,color:0}}),/localPlane range/);
assert.throws(()=>masked.scene.add({name:'bad-blend',geometry:blendGeometry,pattern:{kind:'localPlane',get normal(){masked.body.place({});return [0,1,0];},color:0}}),/busy scene/);
masked.scene.render();
const originalEvaluate=Blend.evaluate,originalBackend=masked.backend.blendEllipsoids,originalNamespace=globalThis.Ayatsuri;
try{
  Blend.evaluate=()=>{throw Error('module replaced');};masked.backend.blendEllipsoids=()=>{throw Error('backend replaced');};
  globalThis.Ayatsuri={};globalThis.AyatsuriBlend={};globalThis.AyatsuriLocalPlane={};
  masked.parent.place({rotation:[.3,.1,0]});masked.scene.evaluate(5);masked.scene.render();classified(blendPacket(masked.scene));
}finally{Blend.evaluate=originalEvaluate;masked.backend.blendEllipsoids=originalBackend;globalThis.Ayatsuri=originalNamespace;globalThis.AyatsuriBlend=Blend;globalThis.AyatsuriLocalPlane=LocalPlane;}
for(const offset of [-4096,4096]){
  const h=masked.scene.add({name:'offset-'+offset,geometry:blendGeometry,pattern:{kind:'localPlane',normal:[0,1,0],offset,feather:4096,color:0}});
  assert.ok(h);masked.scene.evaluate(6);masked.scene.render();
}
const tombstone=masked.body;masked.scene.dispose();unmasked.scene.dispose();
assert.equal(tombstone._raw,null);assert.equal(tombstone._node,null);assert.equal(tombstone._plane,null);
assert.equal(bp.localPlane,null);assert.equal(bp.localPlaneColor,null);assert.equal(bp.localPlaneNormal,null);
assert.throws(()=>tombstone.place({}),/disposed scene/);assert.throws(()=>tombstone.texture({}),/disposed scene/);
// A failure in a tail view allocation occurs inside base.add, before publish.
{
  const s=LocalPlane.scene({backend:{ellipsoid(){},blendEllipsoids(){}},evaluator:TransformReference,geometries:[Blend]});
  const def={name:'retry-blend',geometry:blendGeometry,pattern:{kind:'localPlane',normal:[0,1,0],color:0}};
  const Original=globalThis.Float32Array;
  globalThis.Float32Array=class extends Original{constructor(...args){if(args[1]===316+108*2&&args[2]===5)throw Error('synthetic tail view failure');super(...args);}};
  try{assert.throws(()=>s.add(def),/synthetic tail view failure/);}finally{globalThis.Float32Array=Original;}
  const out={};s.stats(out);assert.equal(out.objects,0);assert.equal(s._plane.records.length,0);assert.equal(s._plane.handles.length,0);
  s.add(def);s.evaluate(0);s.render();s.dispose();
}
console.log('MATERIAL_REGION_JS_PASS owned=true lightInvariant=true poseFollowing=true invalidAtomic=true blendCommonLocal=true tail24=true capturedMethods=true tombstones=true');
