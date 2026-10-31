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
console.log('MATERIAL_REGION_JS_PASS owned=true lightInvariant=true poseFollowing=true invalidAtomic=true');
