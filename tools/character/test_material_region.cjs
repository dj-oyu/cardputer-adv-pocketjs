'use strict';
const assert=require('node:assert/strict');
const Ayatsuri=require('../../apps/character/character.js');
const TransformReference=require('./transform_reference.cjs');
const scene=Ayatsuri.scene({backend:{ellipsoid(){}},evaluator:TransformReference});
const normal=[0,2,0],body=scene.add({name:'body',geometry:{kind:'ellipsoid',radii:[20,30,10]},
  material:{color:0xe47b30,specular:.5},pattern:{kind:'localPlane',normal,offset:.2,feather:.1,color:0xffe8bb}});
normal.fill(NaN);
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
scene.dispose();
console.log('MATERIAL_REGION_JS_PASS owned=true lightInvariant=true poseFollowing=true invalidAtomic=true');
