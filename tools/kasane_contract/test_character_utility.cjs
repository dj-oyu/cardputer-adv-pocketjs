'use strict';
const assert = require('node:assert/strict');
const Ayatsuri = require('../../apps/character/character.js');
const AyatsuriBlend = require('../../apps/character/character_blend.js');
assert.strictEqual(globalThis.Ayatsuri, Ayatsuri);
assert.strictEqual(globalThis.Character, Ayatsuri, 'old name aliases the same visual implementation');
const MeshReference = require('../character/mesh_reference.cjs');
const Motion = require('../../apps/character/motion.js');
const TransformReference = require('../character/transform_reference.cjs');
const analyticScene = options => Ayatsuri.scene({...options, evaluator: TransformReference});
// The rejected coarse experiment remains an independent numerical reference,
// not an implicit production fallback when the native capability is absent.
const backend = {shade: MeshReference.shadeReference, fill() {}};
const scene = MeshReference.scene({backend});
const root = scene.add({name: 'root', geometry: null});
const body = scene.add({name: 'body', parent: root, geometry: {kind: 'ellipsoid', radii: [20, 30, 10]},
  material: {color: 0xcc6600, specular: 0.7, roughness: 0.3},
  pattern: {kind: 'band', axis: 'v', start: 0.6, end: 1, color: 0xffffff}});
const hand = scene.add({name: 'hand', parent: root, position: [30, 0, 0], geometry: {kind: 'ellipsoid', radii: [5, 5, 5]}});
function stats() { const value = {}; scene.stats(value); return value; }
function packet(name) { let result; scene.visit((mesh, key) => { if (key === name) result = mesh; }); return result; }
function same(actual, expected, label) { assert.deepEqual(Array.from(actual), Array.from(expected), label); }
scene.evaluate(0);
let before = stats();
const first = packet('body'), pointsIdentity = first.points, colorsIdentity = first.colors,
  firstAlbedo = first.albedo.slice(), firstWorld = first.world.slice(), firstColors = first.colors.slice();
scene.evaluate(5000);
assert.deepEqual(stats(), before, 'time alone is not an animation or a dirty flag');
body.place({position: [0, 0, 0]}); scene.light({direction: [-0.5, -0.4, 0.8]}); scene.evaluate(5000);
assert.deepEqual(stats(), before, 'equivalent author input is a no-op');
scene.light({direction: [0.6, 0.4, 0.7]}); scene.evaluate(5000);
let after = stats();
assert.equal(after.geometryUpdates, before.geometryUpdates, 'light must not rebuild geometry');
assert.equal(after.albedoUpdates, before.albedoUpdates, 'light must not repaint body markings');
assert.equal(after.shadeUpdates - before.shadeUpdates, 2, 'world light affects both material objects');
same(first.albedo, firstAlbedo, 'white fur marking survives light reversal');
same(first.world, firstWorld, 'light must not move body');
assert.notDeepEqual(Array.from(first.colors), Array.from(firstColors), 'light changes computed material response');
before = after;
const litColors = first.colors.slice();
scene.camera({position: [120, 0, 400]}); scene.evaluate(5000); after = stats();
assert.equal(after.geometryUpdates, before.geometryUpdates, 'camera must not rebuild world geometry');
assert.equal(after.albedoUpdates, before.albedoUpdates, 'camera does not repaint texture');
assert.equal(after.projectionUpdates - before.projectionUpdates, 2);
assert.notDeepEqual(Array.from(first.colors), Array.from(litColors), 'view-dependent shine changes with camera');
before = after;
body.texture({offset: [0, 0.4]}); scene.evaluate(5000); after = stats();
assert.equal(after.geometryUpdates, before.geometryUpdates, 'texture deformation is not body deformation');
assert.equal(after.normalUpdates, before.normalUpdates);
assert.equal(after.projectionUpdates, before.projectionUpdates);
assert.equal(after.albedoUpdates - before.albedoUpdates, 1, 'texture change is local');
assert.equal(after.shadeUpdates - before.shadeUpdates, 1);
assert.notDeepEqual(Array.from(first.albedo), Array.from(firstAlbedo));
before = after;
body.material({roughness: 0.8}); scene.evaluate(5000); after = stats();
assert.equal(after.geometryUpdates, before.geometryUpdates);
assert.equal(after.albedoUpdates, before.albedoUpdates);
assert.equal(after.shadeUpdates - before.shadeUpdates, 1, 'roughness only changes local shading');
before = after;
body.place({rotation: [0, 0.3, 0], scale: [2, 1, 0.5]}); scene.evaluate(5000); after = stats();
assert.equal(after.geometryUpdates - before.geometryUpdates, 1, 'child motion does not rebuild siblings');
assert.equal(after.albedoUpdates, before.albedoUpdates, 'body-attached pattern is unchanged');
assert.strictEqual(packet('body').points, pointsIdentity, 'projected work reused');
assert.strictEqual(packet('body').colors, colorsIdentity, 'shading output reused');
// Normals must be covectors, not multiplied by the position matrix.
for (let i = 0; i < first.n; ++i) {
  const p = 12 * i, x = first.world[p + 3] - first.world[p], y = first.world[p + 4] - first.world[p + 1], z = first.world[p + 5] - first.world[p + 2],
    dot = x * first.normals[3 * i] + y * first.normals[3 * i + 1] + z * first.normals[3 * i + 2];
  const edgeLength = Math.hypot(x, y, z);
  if (edgeLength) assert.ok(Math.abs(dot) / (32767 * edgeLength) < 3e-5,
    'inverse-transpose normal perpendicular to edge within SNORM16 rounding');
}
before = after;
root.place({rotation: [0, 0, 0.1]}); scene.evaluate(5000); after = stats();
assert.equal(after.geometryUpdates - before.geometryUpdates, 3, 'parent dependency propagates to both children');
const validPoints = first.points.slice();
assert.throws(() => body.place({position: [30, 0, 0], scale: [0, 1, 1]}));
scene.evaluate(5000); same(first.points, validPoints, 'invalid placement was not partially applied');
assert.throws(() => body.material({color: 0, roughness: NaN}));
scene.evaluate(5000);
scene.render(); assert.ok(stats().renderedFaces > 0);
assert.throws(() => scene.add({name: 'body', geometry: null}));
const other = analyticScene({backend: {ellipsoid() {}}});
assert.throws(() => other.add({name: 'bad', parent: body, geometry: null}));
assert.throws(() => other.add({name: 'typo', parent: {}, geometry: null}));
assert.throws(() => other.add({name: 'typo', parent: 'root', geometry: null}));
// A failed evaluation cannot leak a partial packet to a presenter.
scene.camera({position: [0, 0, 0], target: [0, 0, 0]});
assert.throws(() => scene.evaluate(0)); assert.throws(() => scene.render());
scene.camera({position: [0, 0, 400]}); scene.evaluate(0); scene.render();
scene.dispose(); scene.dispose(); assert.throws(() => scene.evaluate(0));

// Native analytic rendering must not allocate the per-face experiment buffers.
let analyticCalls = 0, packetScreen, packetNormal, packetTexture;
const compact = analyticScene({backend: {ellipsoid(screen, normal, lighting, material, surface, texture) {
  ++analyticCalls;
  assert.ok(screen instanceof Float32Array && screen.length === 8);
  assert.ok(normal instanceof Float32Array && normal.length === 9);
  assert.ok(surface instanceof Uint16Array && surface.length === 5);
  assert.ok(texture instanceof Float32Array && texture.length === 9);
  assert.strictEqual(screen.buffer, normal.buffer, 'one backing store, not a buffer per field');
  assert.strictEqual(surface.buffer, texture.buffer);
  assert.strictEqual(surface.buffer, material.buffer);
  assert.ok(surface.byteOffset >= texture.byteOffset + texture.byteLength, 'surface must not overlap UV');
  if (packetScreen) assert.strictEqual(screen, packetScreen, 'analytic packet reused');
  packetScreen = screen; packetNormal = normal; packetTexture = texture;
}}});
const compactPart = compact.add({name: 'lit', geometry: {kind: 'ellipsoid', radii: [20, 30, 10]},
  material: {color: 0xff8800, opacity: 0.8, edgeSoftness: 0.2},
  pattern: {kind: 'band', axis: 'v', start: 0.6, end: 1, color: 0xffffff}});
compact.evaluate(0); compact.render();
const compactStats = {}; compact.stats(compactStats);
assert.ok(compactStats.typedBytes < 500, 'one analytic object uses compact descriptors, not a mesh');
assert.equal(compactStats.renderedPatches, 1); assert.equal(compactStats.renderedFaces, 0);
assert.equal(compactStats.shadeSamples, 0, 'descriptor work is not counted as per-pixel shading');
assert.equal(analyticCalls, 1);
same(packetScreen, [20, 0, 0, 120, 0, 30, 0, 75], 'world placement to screen affine');
assert.ok(Math.abs(packetNormal[0] - 1 / 20) < 1e-7 && Math.abs(packetNormal[8] - 1 / 10) < 1e-7);
assert.ok(Math.abs(packetTexture[7] - 0.6) < 1e-6 && packetTexture[8] === 1);
const compactBefore = {}; compact.stats(compactBefore);
compact.light({direction: [0.5, -0.4, 0.8]}); compact.evaluate(1);
compact.stats(compactStats);
assert.equal(compactStats.geometryUpdates, compactBefore.geometryUpdates);
assert.equal(compactStats.projectionUpdates, compactBefore.projectionUpdates);
assert.equal(compactStats.analyticUpdates - compactBefore.analyticUpdates, 1);
compact.stats(compactBefore);
compact.evaluate(10000); compact.stats(compactStats);
assert.deepEqual(compactStats, compactBefore, 'runtime time-only evaluate is a no-op');
compact.camera({position: [120, 0, 400]}); compact.evaluate(10000); compact.stats(compactStats);
assert.equal(compactStats.geometryUpdates, compactBefore.geometryUpdates, 'runtime camera does not rebuild world geometry');
assert.equal(compactStats.albedoUpdates, compactBefore.albedoUpdates, 'runtime camera does not repaint markings');
assert.equal(compactStats.projectionUpdates - compactBefore.projectionUpdates, 1);
compact.stats(compactBefore);
compactPart.texture({offset: [0, 0.2]}); compact.evaluate(10000); compact.stats(compactStats);
assert.equal(compactStats.geometryUpdates, compactBefore.geometryUpdates);
assert.equal(compactStats.normalUpdates, compactBefore.normalUpdates);
assert.equal(compactStats.projectionUpdates, compactBefore.projectionUpdates);
assert.equal(compactStats.albedoUpdates - compactBefore.albedoUpdates, 1);
compact.stats(compactBefore);
compactPart.material({roughness: 0.8}); compact.evaluate(10000); compact.stats(compactStats);
assert.equal(compactStats.geometryUpdates, compactBefore.geometryUpdates);
assert.equal(compactStats.albedoUpdates, compactBefore.albedoUpdates);
assert.equal(compactStats.analyticUpdates - compactBefore.analyticUpdates, 1);
compactPart.place({scale: [2, 1, 0.5]}); compact.evaluate(1); compact.render();
assert.ok(Math.abs(packetNormal[0] - 1 / 40) < 1e-7 && Math.abs(packetNormal[8] - 1 / 5) < 1e-7);
assert.throws(() => compact.add({name: 'unsupported', geometry: {kind: 'ellipsoid', radii: [1, 1, 1]},
  pattern: {kind: 'image', width: 1, height: 1, pixels: new Uint16Array(1)}}), /image pattern/);
const validCompactScreen = packetScreen.slice();
assert.throws(() => compactPart.place({position: [90, 0, 0], scale: [0, 1, 1]}));
compact.evaluate(1); compact.render(); same(packetScreen, validCompactScreen, 'runtime invalid setter is atomic');
compact.camera({position: [0, 0, 0], target: [0, 0, 0]});
assert.throws(() => compact.evaluate(1)); assert.throws(() => compact.render());
compact.camera({position: [0, 0, 400]}); compact.evaluate(1); compact.render();
assert.throws(() => compact.add({name: 'bad-parent', parent: {}, geometry: null}));
compact.dispose();
assert.throws(() => Ayatsuri.scene({backend: {fill() {}}}), /analytic hone backend/, 'no silent coarse device fallback');
const hierarchy = analyticScene({backend: {ellipsoid() {}}});
const group = hierarchy.add({name: 'group', geometry: null}), leaf = hierarchy.add({name: 'leaf', parent: group, geometry: {kind: 'ellipsoid', radii: [1, 2, 3]}});
const groupKeys=Object.keys(group._node).sort();
for(const key of ['materialData','color','thickness','opacity','edgeSoftness','pattern','textureData',
    'albedoVersion','evaluatedAlbedo','shadingVersion','evaluatedShading'])
  assert.ok(!Object.hasOwn(group._node,key),'group omits analytic-only '+key);
hierarchy.add({name: 'sibling', parent: group, geometry: {kind: 'ellipsoid', radii: [1, 1, 1]}});
const h0 = {}, h1 = {}; hierarchy.evaluate(0); hierarchy.stats(h0);
assert.strictEqual(leaf.place, group.place, 'shared visual handle method, no per-node closure');
assert.strictEqual(leaf.material, group.material);
assert.throws(() => group.material({color: 0xffffff}), /group has no material/);
assert.throws(() => group.texture({offset: [0,0]}), /group has no texture/);
leaf.place({position: [1, 0, 0]}); hierarchy.evaluate(0); hierarchy.stats(h1);
assert.equal(h1.geometryUpdates - h0.geometryUpdates, 1, 'runtime local placement is local');
hierarchy.stats(h0); group.place({rotation: [0, 0, 0.1]}); hierarchy.evaluate(0); hierarchy.stats(h1);
assert.equal(h1.geometryUpdates - h0.geometryUpdates, 3, 'runtime parent dependencies reach both children');
hierarchy.camera({position:[0,0,0],target:[0,0,0]});
assert.throws(()=>hierarchy.evaluate(0));
assert.deepEqual(Object.keys(group._node).sort(),groupKeys,'failure invalidation does not add analytic fields to groups');
hierarchy.camera({position:[0,0,400]});hierarchy.evaluate(0);hierarchy.render();
assert.deepEqual(Object.keys(group._node).sort(),groupKeys,'recovery keeps compact group storage');
hierarchy.dispose();

// A backend/visitor cannot mutate or reenter its active scene. Other scenes
// remain independent, and callback failure releases the per-scene guard.
let onDraw = () => {};
const guarded = analyticScene({backend: {ellipsoid() { onDraw(); }}});
const guardedPart = guarded.add({name: 'part', geometry: {kind: 'ellipsoid', radii: [2, 2, 2]}});
guarded.evaluate(0);
const isolated = analyticScene({backend: {ellipsoid() {}}});
const isolatedPart = isolated.add({name: 'part', geometry: {kind: 'ellipsoid', radii: [1, 1, 1]}});
isolated.evaluate(0);
onDraw = () => {
  for (const action of [() => guarded.render(), () => guarded.evaluate(0), () => guarded.dispose(),
    () => guardedPart.place({position: [1, 0, 0]})]) assert.throws(action, /busy scene/);
  isolatedPart.place({position: [2, 0, 0]}); isolated.evaluate(0); isolated.render();
};
guarded.render();
guardedPart.place({position: [1, 0, 0]});
assert.throws(() => guarded.render(), /evaluate before render/, 'mutation requires a fresh evaluation');
guarded.evaluate(0);
guardedPart.material({color: 0x225588, opacity: 0.5});
assert.throws(() => guarded.render(), /evaluate before render/, 'material changes require matching derived surface evaluation');
guarded.evaluate(0); guarded.render();
assert.throws(() => guarded.visit(() => guarded.light({color: 0})), /busy scene/);
onDraw = () => { throw Error('backend failure'); };
assert.throws(() => guarded.render(), /backend failure/);
onDraw = () => {}; guarded.render();
assert.throws(() => guardedPart.material({color: 0, opacity: NaN}));
guarded.render(); // Rejected input did not invalidate or change the packet.
assert.throws(() => guarded.add({name: 'bad', position: [1, 2, 3], geometry: {kind: 'ellipsoid', radii: [1, 1, 1]},
  pattern: {kind: 'unknown'}}));
guarded.render();
isolated.dispose(); guarded.render(); guarded.dispose(); guarded.dispose();
assert.throws(() => guardedPart.visible(true), /disposed scene/);

// Shared transform evaluation stays within the scene's synchronous guard.
assert.throws(() => Ayatsuri.scene({backend: {ellipsoid() {}}}), /shared ayatsuri evaluator/,
  'a device scene never silently falls back to a separate JS transform implementation');
let composeCalls=0, composeHook=()=>{}, renderCalls=0;
const delegated=Ayatsuri.scene({backend:{ellipsoid(){++renderCalls;}},evaluator:{compose(pose,parent,out){
  ++composeCalls;composeHook();TransformReference.compose(pose,parent,out);
}}});
const delegatedRoot=delegated.add({name:'root',geometry:null}),
  delegatedPart=delegated.add({name:'leaf',parent:delegatedRoot,geometry:{kind:'ellipsoid',radii:[2,3,4]}});
composeHook=()=>{
  for(const action of [()=>delegated.evaluate(0),()=>delegated.render(),()=>delegated.dispose(),
    ()=>delegatedPart.place({position:[3,0,0]})]) assert.throws(action,/busy scene/);
};
delegated.evaluate(0);delegated.render();assert.equal(composeCalls,2);
delegated.evaluate(5000);assert.equal(composeCalls,2,'time alone does not call the shared transform kernel');
delegated.light({color:0x88aacc});delegated.evaluate(5000);assert.equal(composeCalls,2,'lighting does not call the transform kernel');
delegatedRoot.place({position:[3,0,0]});delegated.evaluate(5000);
assert.equal(composeCalls,4,'parent update evaluates the parent before its child');
const successfulMatrix=delegatedPart._node.matrix.slice(0,12);
composeHook=()=>{throw Error('evaluator failure');};
delegatedPart.place({position:[7,0,0]});
assert.throws(()=>delegated.evaluate(5000),/evaluator failure/);
same(delegatedPart._node.matrix.slice(0,12),successfulMatrix,'failed kernel did not publish world output');
const renderedBefore=renderCalls;
assert.throws(()=>delegated.render(),/evaluate before render/);
assert.throws(()=>delegated.visit(()=>{}),/evaluate before render/);
assert.equal(renderCalls,renderedBefore,'failed evaluation reaches no render backend');
composeHook=()=>{};delegated.evaluate(5000);delegated.render();
assert.equal(delegatedPart._node.matrix[3],10,'retry uses the latest authored pose and parent');
delegated.dispose();

// Packed native transform path owns one stable 21-float view: world matrix
// first, authored TRS last. Only this host reference creates temporary views.
let packedComposeCalls=0,packedComposeFailure=false;
const packedTransforms=Ayatsuri.scene({backend:{ellipsoid(){}},evaluator:{
  compose(){throw new Error('legacy compose must not run');},
  composeNode(node,parent){
    ++packedComposeCalls;
    if(packedComposeFailure)throw new Error('packed evaluator failure');
    assert.equal(node.length,21);
    const pose=new Float32Array(node.buffer,node.byteOffset+48,9),
      world=new Float32Array(node.buffer,node.byteOffset,12),
      parentWorld=parent?new Float32Array(parent.buffer,parent.byteOffset,12):null;
    TransformReference.compose(pose,parentWorld,world);
  }
}});
const packedRoot=packedTransforms.add({name:'root',geometry:null,position:[2,0,0]}),
  packedLeaf=packedTransforms.add({name:'leaf',parent:packedRoot,position:[3,0,0],geometry:{kind:'ellipsoid',radii:[2,3,4]}});
for(const handle of [packedRoot,packedLeaf]){
  assert.equal(handle._node.matrix.length,21);
  assert.ok(!Object.hasOwn(handle._node,'pose')&&!Object.hasOwn(handle._node,'compatMatrix'),
    'packed path never allocates the two legacy compatibility views');
}
packedTransforms.evaluate(0);packedTransforms.render();assert.equal(packedComposeCalls,2);
assert.equal(packedLeaf._node.matrix[3],5);assert.equal(packedLeaf._node.matrix[12],3);
const packedWorld=packedLeaf._node.matrix.slice(0,12),packedStableView=packedLeaf._node.matrix;
packedLeaf.place({position:[7,0,0]});packedComposeFailure=true;
assert.throws(()=>packedTransforms.evaluate(1),/packed evaluator failure/);
same(packedLeaf._node.matrix.slice(0,12),packedWorld,'packed failure leaves last world matrix intact');
assert.equal(packedLeaf._node.matrix[12],7,'authored pose remains the requested input');
assert.throws(()=>packedTransforms.render(),/evaluate before render/);
packedComposeFailure=false;packedTransforms.evaluate(1);packedTransforms.render();
assert.equal(packedLeaf._node.matrix[3],9);assert.strictEqual(packedLeaf._node.matrix,packedStableView);
assert.ok(!Object.hasOwn(packedRoot._node,'pose')&&!Object.hasOwn(packedLeaf._node,'compatMatrix'));
packedTransforms.dispose();


// Packed native submission avoids retaining four compatibility views per node.
let packedBuffer, packedLight, packedCalls=0;
const packedOnly=analyticScene({backend:{ellipsoidPacked(storage,offset,light){
  assert.ok(storage instanceof ArrayBuffer);assert.equal(offset,96);assert.equal(storage.byteLength,264);
  if(packedBuffer)assert.strictEqual(storage,packedBuffer);
  packedBuffer=storage;packedLight=light;++packedCalls;
}}});
const packedPart=packedOnly.add({name:'packed',geometry:{kind:'ellipsoid',radii:[20,30,10]},
  position:[3,4,5],material:{color:0xff8800,opacity:.8,edgeSoftness:.2},
  pattern:{kind:'band',axis:'v',start:.6,end:1,color:0xffffff}});
const packedPatch=packedPart._node.analytic;
for(const name of ['radii','screen','normalMatrix','texture','material'])assert.equal(Object.hasOwn(packedPatch,name),false);
packedOnly.evaluate(0);packedOnly.render();packedOnly.evaluate(1);packedOnly.render();
assert.equal(packedCalls,2);assert.equal(packedPart._node.pose.buffer,packedBuffer);
for(const name of ['radii','screen','normalMatrix','texture','material'])assert.equal(Object.hasOwn(packedPatch,name),false,
  'native packed frames never allocate compatibility views');
assert.equal(Object.hasOwn(packedPart._node,'textureData'),false);
assert.equal(Object.hasOwn(packedPart._node,'materialData'),false);
assert.equal(packedPatch.work[63],1);assert.equal(packedPatch.work[64],1);
assert.equal(packedPatch.surface.byteOffset,232);
same(new Float32Array(packedBuffer,96,8),[20,0,0,123,0,30,0,79]);
const packedMaterial=new Float32Array(packedBuffer,164,8),packedTexture=new Float32Array(packedBuffer,196,9);
assert.equal(packedMaterial[0],.25);assert.equal(packedTexture[6],Math.fround(.2));
assert.equal(packedPatch.surface[2],204);assert.ok(packedLight instanceof Float32Array);
let borrowed;
packedOnly.visit(packet=>{borrowed=packet;});
assert.strictEqual(borrowed,packedPatch);assert.equal(borrowed.radii.byteOffset,84);
assert.equal(borrowed.screen.byteOffset,96);assert.equal(borrowed.normalMatrix.byteOffset,128);
assert.equal(borrowed.texture.byteOffset,196);
assert.equal(borrowed.material.byteOffset,164);assert.equal(borrowed.material.length,8);
const stableScreen=borrowed.screen,stableNormal=borrowed.normalMatrix,stableTexture=borrowed.texture,stableMaterial=borrowed.material;
packedOnly.visit(packet=>{assert.strictEqual(packet.screen,stableScreen);assert.strictEqual(packet.normalMatrix,stableNormal);
  assert.strictEqual(packet.texture,stableTexture);assert.strictEqual(packet.material,stableMaterial);});
packedPart.texture({offset:[0,.2]});assert.throws(()=>packedOnly.render(),/evaluate before render/);
packedOnly.evaluate(1);packedOnly.render();assert.strictEqual(packedPatch.texture,stableTexture);
assert.equal(packedPart._node.analytic.work[62],Math.fround(.2));
packedOnly.dispose();

// Lazy compatibility publishes all five views only after every allocation succeeds.
for(let failAt=1;failAt<=5;++failAt){
  const candidate=analyticScene({backend:{ellipsoidPacked(){}}}),
    part=candidate.add({name:'compat-oom',geometry:{kind:'ellipsoid',radii:[2,3,4]}});
  candidate.evaluate(0);candidate.render();
  const patch=part._node.analytic,buffer=patch.work.buffer,RealFloat32Array=globalThis.Float32Array;
  let allocations=0;
  globalThis.Float32Array=new Proxy(RealFloat32Array,{construct(target,args){
    if(++allocations===failAt)throw Error('injected compatibility OOM');
    return Reflect.construct(target,args,target);
  }});
  try{assert.throws(()=>candidate.visit(()=>{}),/injected compatibility OOM/);}
  finally{globalThis.Float32Array=RealFloat32Array;}
  assert.equal(allocations,failAt);
  for(const name of ['radii','screen','normalMatrix','texture','material'])assert.equal(Object.hasOwn(patch,name),false,
    'failed lazy allocation publishes no partial compatibility packet');
  candidate.render(); // Failed visit released the guard; packed rendering still works.
  candidate.visit(packet=>{
    for(const name of ['radii','screen','normalMatrix','texture','material'])assert.strictEqual(packet[name].buffer,buffer);
  });
  const views=[patch.radii,patch.screen,patch.normalMatrix,patch.texture,patch.material];
  globalThis.Float32Array=new Proxy(RealFloat32Array,{construct(){throw Error('initialized packet allocated again');}});
  try{candidate.visit(packet=>{
    assert.strictEqual(packet.radii,views[0]);assert.strictEqual(packet.screen,views[1]);
    assert.strictEqual(packet.normalMatrix,views[2]);assert.strictEqual(packet.texture,views[3]);
    assert.strictEqual(packet.material,views[4]);
  });candidate.render();}
  finally{globalThis.Float32Array=RealFloat32Array;}
  candidate.dispose();
}

// A blended volume is one visual object, one native call and one alpha layer.
let blendCalls=0, blendComposeCalls=0, blendPacket, blendLighting, blendMaterial, blendSurface, blendControls;
let blendDrawHook=()=>{};
const geometryExtensions=[AyatsuriBlend];
const blended=Ayatsuri.scene({geometries:geometryExtensions,backend:{ellipsoid(){throw Error('wrong backend');},
  blendEllipsoids(lobes,light,material,surface,controls){
    ++blendCalls;blendDrawHook();
    assert.equal(lobes.length,42);assert.equal(light.length,9);assert.equal(material.length,11);
    assert.equal(surface.length,3);assert.equal(controls.length,2);
    assert.ok(lobes instanceof Float32Array&&surface instanceof Uint16Array);
    assert.strictEqual(lobes.buffer,material.buffer);assert.strictEqual(material.buffer,surface.buffer);
    assert.strictEqual(surface.buffer,controls.buffer);
    if(blendPacket){assert.strictEqual(lobes,blendPacket);assert.strictEqual(material,blendMaterial);
      assert.strictEqual(surface,blendSurface);assert.strictEqual(controls,blendControls);}
    blendPacket=lobes;blendLighting=light;blendMaterial=material;blendSurface=surface;blendControls=controls;
  }},evaluator:{compose(p,parent,out){++blendComposeCalls;TransformReference.compose(p,parent,out);}}});
geometryExtensions.length=0;delete globalThis.AyatsuriBlend;
const blendParent=blended.add({name:'root',geometry:null,position:[10,5,3]});
const sourceLobes=[{position:[-5,0,0],radii:[10,20,30]},{position:[5,0,2],radii:[8,12,16]}];
const volume=blended.add({name:'volume',parent:blendParent,
  geometry:{kind:'blendEllipsoids',lobes:sourceLobes,blend:8},
  material:{color:0xffffff,ambientColor:0x80c0ff,opacity:.5,edgeSoftness:.2}});
sourceLobes[0].position[0]=999;sourceLobes[0].radii[0]=999;
blended.evaluate(0);blended.render();
assert.equal(blendCalls,1);assert.equal(blendComposeCalls,2,'one C TRS per node, never per lobe');
same(blendPacket.slice(0,8),[10,0,0,125,0,20,0,80],'copied local lobe projected through parent');
same(blendPacket.slice(17,21),[0,0,30,3],'depth is world-distance along view relative to target');
same(blendPacket.slice(38,42),[0,0,16,5],'each lobe has its own transformed center and basis');
assert.equal(blendSurface[0],0xffff);assert.equal(blendSurface[1],128);assert.equal(blendSurface[2],128);
same(blendMaterial.slice(8),new Float32Array([128/255,192/255,1]),'ambient RGB is distinct from albedo');
assert.equal(blendControls[0],8);assert.equal(blendControls[1],Math.fround(.2));
const blendStats0={},blendStats1={};blended.stats(blendStats0);
assert.equal(blendStats0.renderedPatches,1);assert.equal(blendStats0.renderedFaces,0);assert.equal(blendStats0.shadeSamples,0);
assert.equal(blendStats0.copiedBytes,0,'utility stores descriptors; native snapshot copying is measured by native');
const stablePacket=blendPacket.slice();
blended.light({direction:[.8,-.2,.5]});blended.evaluate(10);blended.render();blended.stats(blendStats1);
assert.equal(blendComposeCalls,2);same(blendPacket,stablePacket,'light does not alter geometric packet');
assert.equal(blendStats1.normalUpdates,blendStats0.normalUpdates);
assert.equal(blendStats1.projectionUpdates,blendStats0.projectionUpdates);
assert.equal(blendStats1.analyticUpdates-blendStats0.analyticUpdates,1);
blended.stats(blendStats0);volume.material({ambientColor:0x1122aa,opacity:.7});
assert.throws(()=>blended.render(),/evaluate before render/);
blended.evaluate(10);blended.render();blended.stats(blendStats1);
assert.equal(blendStats1.geometryUpdates,blendStats0.geometryUpdates);
assert.equal(blendStats1.projectionUpdates,blendStats0.projectionUpdates);
assert.equal(blendStats1.albedoUpdates,blendStats0.albedoUpdates);
same(blendMaterial.slice(8),new Float32Array([17/255,34/255,170/255]));
assert.equal(blendSurface[1],179);
const materialBefore=blendMaterial.slice();
assert.throws(()=>volume.material({color:0,ambientColor:0x1000000}),/RGB24/);
blended.render();same(blendMaterial,materialBefore,'invalid shading setter validates before changing material');
for(const def of [
  {kind:'blendEllipsoids',lobes:[]},
  {kind:'blendEllipsoids',lobes:Array(9).fill({position:[0,0,0],radii:[1,1,1]})},
  {kind:'blendEllipsoids',lobes:[{position:[0,0,0],radii:[0,1,1]}]},
  {kind:'blendEllipsoids',lobes:[{position:[NaN,0,0],radii:[1,1,1]}]},
  {kind:'blendEllipsoids',lobes:[{position:[0,0,0],radii:[1,1,1]}],blend:129}
]) {assert.throws(()=>blended.add({name:'invalid',geometry:def}));blended.render();}
assert.throws(()=>blended.add({name:'pattern',geometry:{kind:'blendEllipsoids',lobes:sourceLobes},
  pattern:{kind:'checker',color:0}}),/no pattern or texture/);blended.render();
assert.throws(()=>volume.texture({offset:[0,.2]}),/no texture/);blended.render();
blendParent.place({scale:[-2,1,.5]});blended.evaluate(10);blended.render();
assert.ok(Math.abs(blendPacket[8]+.05)<1e-7&&Math.abs(blendPacket[16]-1/15)<1e-7,
  'inverse transpose supports reflection and nonuniform scale');
const beforeCamera=blendPacket.slice();blended.stats(blendStats0);
blended.camera({position:[400,0,0]});blended.evaluate(10);blended.render();blended.stats(blendStats1);
assert.equal(blendComposeCalls,4);assert.equal(blendStats1.normalUpdates,blendStats0.normalUpdates);
assert.equal(blendStats1.projectionUpdates-blendStats0.projectionUpdates,1);
same(blendPacket.slice(8,17),beforeCamera.slice(8,17),'camera does not rebuild world normals');
same(blendPacket.slice(17,21),[-20,0,0,20],'camera changes view depth, including reflected basis');
blendDrawHook=()=>{assert.throws(()=>volume.material({opacity:.2}),/busy scene/);
  assert.throws(()=>blended.evaluate(0),/busy scene/);};blended.render();blendDrawHook=()=>{};
volume.place({scale:[10000,1,1]});assert.throws(()=>blended.evaluate(10),/packet range/);
assert.throws(()=>blended.render(),/evaluate before render/);
volume.place({scale:[1,1,1]});blended.evaluate(10);blended.render();
blended.stats(blendStats0);blended.evaluate(1000);blended.stats(blendStats1);
assert.deepEqual(blendStats1,blendStats0,'time alone reuses all blend descriptors');
blended.dispose();assert.equal(volume._node.geometryExtension,null,'dispose releases descriptor even while author retains handle');
assert.throws(()=>volume.material({opacity:1}),/disposed scene/);
const explicitOnly=analyticScene({backend:{ellipsoid(){},blendEllipsoids(){}}});
assert.throws(()=>explicitOnly.add({name:'implicit',geometry:{kind:'blendEllipsoids',lobes:sourceLobes}}),/geometry must/);
explicitOnly.dispose();
const noBlend=analyticScene({geometries:[AyatsuriBlend],backend:{ellipsoid(){}}});
const ordinary=noBlend.add({name:'ordinary',geometry:{kind:'ellipsoid',radii:[1,1,1]}});
assert.equal(Object.hasOwn(ordinary._node,'geometryExtension'),false,'ordinary nodes keep the original compact property shape');
noBlend.evaluate(0);assert.throws(()=>ordinary.material({ambientColor:0}),/requires blendEllipsoids/);noBlend.render();
assert.throws(()=>noBlend.add({name:'unsupported',geometry:{kind:'blendEllipsoids',lobes:sourceLobes}}),/backend required/);
noBlend.render();noBlend.dispose();
assert.equal(Object.hasOwn(ordinary._node,'geometryExtension'),false,'dispose does not add extension state to ordinary nodes');

const track = Motion.track([0, 2, 10, 4, 20, 6], {stride: 2, timeUnit: 'frame', interpolation: 'smooth'}), pose = [0, 0];
Motion.sample(track, 5, pose); same(pose, [5, 3], 'frame positions, not elapsed seconds');
Motion.sample(track, -1, pose); same(pose, [0, 2], 'clamped start');
Motion.sample(track, 100, pose); same(pose, [20, 6], 'clamped end');
assert.throws(() => Motion.track([0, 2, 0, 3], {stride: 2, timeUnit: 'frame'}));
// The model combines expression and closure; replacing open itself would fail.
let expression = 0.5;
const blink = Motion.track([0, 0, 80, 1, 120, 1, 240, 0], {stride: 2, timeUnit: 'ms', interpolation: 'smooth'});
Motion.sample(blink, 80, pose); assert.equal(expression * (1 - pose[1]), 0);
expression = 0.3; Motion.sample(blink, 240, pose); assert.equal(expression * (1 - pose[1]), 0.3);
console.log('CHARACTER_UTILITY PASS: analytic runtime dependency isolation/compact borrowed packets/blended-volume ownership and projection/failure recovery; coarse numerical reference; motion sampling');
