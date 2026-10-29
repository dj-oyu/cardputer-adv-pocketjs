'use strict';
const assert = require('node:assert/strict');
const Character = require('../../apps/character/character.js');
const MeshReference = require('../character/mesh_reference.cjs');
const Motion = require('../../apps/character/motion.js');
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
const other = Character.scene({backend: {ellipsoid() {}}});
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
const compact = Character.scene({backend: {ellipsoid(screen, normal, lighting, material, surface, texture) {
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
assert.throws(() => Character.scene({backend: {fill() {}}}), /analytic shapes backend/, 'no silent coarse device fallback');
const hierarchy = Character.scene({backend: {ellipsoid() {}}});
const group = hierarchy.add({name: 'group', geometry: null}), leaf = hierarchy.add({name: 'leaf', parent: group, geometry: {kind: 'ellipsoid', radii: [1, 2, 3]}});
hierarchy.add({name: 'sibling', parent: group, geometry: {kind: 'ellipsoid', radii: [1, 1, 1]}});
const h0 = {}, h1 = {}; hierarchy.evaluate(0); hierarchy.stats(h0);
assert.strictEqual(leaf.place, group.place, 'shared visual handle method, no per-node closure');
assert.strictEqual(leaf.material, group.material);
assert.throws(() => group.material({color: 0xffffff}), /group has no material/);
leaf.place({position: [1, 0, 0]}); hierarchy.evaluate(0); hierarchy.stats(h1);
assert.equal(h1.geometryUpdates - h0.geometryUpdates, 1, 'runtime local placement is local');
hierarchy.stats(h0); group.place({rotation: [0, 0, 0.1]}); hierarchy.evaluate(0); hierarchy.stats(h1);
assert.equal(h1.geometryUpdates - h0.geometryUpdates, 3, 'runtime parent dependencies reach both children');
hierarchy.dispose();

// A backend/visitor cannot mutate or reenter its active scene. Other scenes
// remain independent, and callback failure releases the per-scene guard.
let onDraw = () => {};
const guarded = Character.scene({backend: {ellipsoid() { onDraw(); }}});
const guardedPart = guarded.add({name: 'part', geometry: {kind: 'ellipsoid', radii: [2, 2, 2]}});
guarded.evaluate(0);
const isolated = Character.scene({backend: {ellipsoid() {}}});
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
console.log('CHARACTER_UTILITY PASS: analytic runtime dependency isolation/compact borrowed packets/failure recovery; coarse numerical reference; motion sampling');
