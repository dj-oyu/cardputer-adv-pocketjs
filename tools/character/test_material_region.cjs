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
for(const pattern of [
  {normal:[0,0,0]}, {normal:[0,NaN,0]}, {normal:[0,1,0],offset:2.1},
  {normal:[0,1,0],feather:1e-5}, {normal:[0,1,0],feather:Infinity}, {normal:[0,1,0],color:0x1000000}
]){
  assert.throws(()=>scene.add({geometry:{kind:'ellipsoid',radii:[1,1,1]},pattern:{kind:'localPlane',color:0xffffff,...pattern}}));
  assert.deepEqual(stats(),beforeInvalid,'failed construction does not publish a node');
}
scene.render();
scene.dispose();
console.log('MATERIAL_REGION_JS_PASS owned=true lightInvariant=true poseFollowing=true invalidAtomic=true');
