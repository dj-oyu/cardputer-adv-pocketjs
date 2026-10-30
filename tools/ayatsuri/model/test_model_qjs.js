'use strict';
function assert(value, message) { if (!value) throw Error(message); }
function rejects(fn, code) {
  let caught = false;
  try { fn(); } catch (e) { caught = !code || String(e).includes(code); }
  assert(caught, 'expected rejection ' + code);
}
function equal(a, b) { return a.length === b.length && a.every((x, i) => Object.is(x, b[i])); }
const model = modelOpen(sourceBuffer), count = model.count;
assert(count > 1, 'fixture has hierarchy');
const coldBytes = sourceBuffer.byteLength, initially = model.stats({});
assert(!initially.closed && !initially.released && initially.instances === 0 && initially.ownedBytes === coldBytes && initially.staticBytes === 0, 'owned model initial lifetime');
let modelGuarded = false;
model.stats({set closed(value) { rejects(() => model.dispose(), 'BUSY'); modelGuarded = value === false; }});
assert(modelGuarded, 'model stats output busy guard');
rejects(() => model.stats({set closed(value) { throw Error('model setter failure'); }}));
assert(!model.stats({}).closed, 'model stats exception releases busy');
const noActors = ownedModel(); noActors.dispose(); noActors.dispose();
assert(noActors.stats({}).released && noActors.stats({}).ownedBytes === 0, 'owned bytes released with no actors despite live Model object');
const staticOnly = staticModel();
assert(staticOnly.stats({}).staticBytes === coldBytes && staticOnly.stats({}).ownedBytes === 0, 'static span attached without ownership');
staticOnly.dispose(); assert(staticOnly.stats({}).released && staticOnly.stats({}).staticBytes === 0, 'static span detached without freeing flash');
rejects(() => modelOpen(new ArrayBuffer(4)), 'INVALID_ARGUMENT');
if (typeof SharedArrayBuffer === 'function') rejects(() => modelOpen(new SharedArrayBuffer(64)), 'INVALID_ARGUMENT');
const detached = new ArrayBuffer(8);
if (typeof detached.transfer === 'function') { detached.transfer(); rejects(() => modelOpen(detached)); }
rejects(() => { model.count = 0; });
assert(model.node(firstName) === 0, 'stable named node');
rejects(() => model.node({toString() { throw Error('must not coerce'); }}), 'INVALID_ARGUMENT');
const a = model.instance(), b = model.instance(), before = new Float32Array(9), other = new Float32Array(9);
assert(model.stats({}).instances === 2, 'two live actors counted');
a.readPose(0, before); b.readPose(0, other);
assert(equal(before, other), 'shared model independent initialization');
new Uint8Array(sourceBuffer).fill(0);
// Cold bytes are an owned snapshot, not the caller's now-corrupt buffer.
const third = model.instance(); third.dispose(); third.dispose();
rejects(() => third.readPose(0, other), 'CLOSED');
const dirty = new Uint8Array(count), poses = new Float32Array(count * 9), colors = new Uint32Array(count), opacity = new Float64Array(count);
if (typeof SharedArrayBuffer === 'function') rejects(() => a.targets(new Uint8Array(new SharedArrayBuffer(count)), poses, colors, opacity));
dirty[0] = 1; poses[0] = before[0] + 3; poses[1] = before[1]; poses[2] = before[2];
a.targets(dirty, poses, colors, opacity);
a.readPose(0, other); assert(other[0] === before[0] + 3, 'selected position copied');
dirty[count - 1] = 2; poses[0] = 123; poses[(count - 1) * 9 + 3] = NaN;
rejects(() => a.targets(dirty, poses, colors, opacity));
a.readPose(0, other); assert(other[0] === before[0] + 3, 'late invalid batch leaves earlier node unchanged');
poses.fill(NaN); dirty.fill(255); colors.fill(0xffffffff); opacity.fill(NaN);
a.readPose(0, other); assert(other[0] === before[0] + 3, 'batch never retained');
b.readPose(0, other); assert(equal(other, before), 'instance separation');
const camera = new Float32Array([0,0,300,0,0,0,1,120,67.5]), light = new Float32Array([0,0,1,1,1,1]);
a.evaluate(camera, light); a.submit();
const stable = snapshotHash();
rejects(() => a.targets(dirty, poses, colors, opacity));
clearPackets(); a.submit(); assert(snapshotHash() === stable, 'rejected batch retains publishable packets');
const stats = {}; a.stats(stats);
assert(stats.nodes === count && stats.arenaBytes > 0 && stats.evaluated, 'explicit stats');
let guarded = false;
a.stats({set nodes(value) { rejects(() => a.dispose(), 'BUSY'); guarded = value === count; }});
assert(guarded, 'busy guard before output setter');
rejects(() => a.stats({set nodes(value) { throw Error('setter failure'); }}));
a.stats(stats); assert(stats.evaluated, 'throwing output setter releases busy');
const badCamera = camera.slice(); badCamera[6] = Infinity;
rejects(() => a.evaluate(badCamera, light));
rejects(() => a.submit(), 'CLOSED');
a.evaluate(camera, light);
clearPackets(); a.submit(); const packetSnapshot = snapshotHash();
forceSubmitFailure(2); rejects(() => a.submit());
assert(snapshotHash() === packetSnapshot, 'whole multi-packet candidate rollback');
forceSubmitFailure(0);
model.dispose(); model.dispose();
assert(model.stats({}).closed && model.stats({}).instances === 2 && !model.stats({}).released && model.stats({}).ownedBytes === coldBytes, 'close retains cold bytes for live actors');
rejects(() => model.instance(), 'CLOSED');
rejects(() => model.node(firstName), 'CLOSED');
// Closing Model prohibits new owners; its two existing actors remain usable.
a.evaluate(camera, light); b.evaluate(camera, light);
clearPackets(); b.submit();
a.dispose(); rejects(() => a.evaluate(camera, light), 'CLOSED');
rejects(() => a.stats({}), 'CLOSED');
assert(model.stats({}).instances === 1 && model.stats({}).ownedBytes === coldBytes, 'first actor release protects last actor');
const beforeColdRelease = snapshotHash();
b.dispose();
assert(model.stats({}).instances === 0 && model.stats({}).released && model.stats({}).ownedBytes === 0, 'last actor releases closed owned bytes immediately');
assert(snapshotHash() === beforeColdRelease, 'published numeric snapshot survives cold release');
const duringStats = ownedModel(), duringActor = duringStats.instance(); duringStats.dispose();
const entrySnapshot = {set closed(value) { assert(value === true, 'closed snapshot'); duringActor.dispose(); }};
duringStats.stats(entrySnapshot);
assert(entrySnapshot.instances === 1 && entrySnapshot.ownedBytes === coldBytes && !entrySnapshot.released, 'stats remains entry snapshot across actor release callback');
assert(duringStats.stats({}).released && duringStats.stats({}).instances === 0, 'callback release completed safely');
// A user expando may close a cycle across the native retained Model value.
let cycleModel = ownedModel(), cycleActor = cycleModel.instance();
cycleModel.actor = cycleActor; cycleActor = null; cycleModel = null;
let closedCycle = ownedModel(), closedActor = closedCycle.instance();
closedCycle.actor = closedActor; closedCycle.dispose(); closedActor = null; closedCycle = null;
assert(typeof staticModel === 'function', 'static path exists');
// Load and exercise the actual checked-in visual wrapper against the real
// native classes. A JS-only mock cannot establish the 8/16 visibility ABI.
const visualModel = staticModel(), visual = AyatsuriNative.scene({model: visualModel}), part = visual.handle(1);
function visualFrame(time) { visual.evaluate(time); clearPackets(); visual.render(); return packetCount(); }
const visualBaseline = visualFrame(0);
assert(visualBaseline === visual.stats({}).packets && visualBaseline > 1, 'wrapper native baseline');
part.visible(false); assert(visualFrame(1) === visualBaseline - 1, 'wrapper hide');
part.visible(true); assert(visualFrame(2) === visualBaseline, 'wrapper show');
part.visible(false); assert(visualFrame(3) === visualBaseline - 1, 'wrapper hide again');
part.place({position:[2,3,4]}); part.material({color:0x80b0ff,opacity:0.7});
part.visible(true); part.visible(false);
assert((visual.mask[1] & 127) === (1|16|32|64), 'visibility replaces only its own dirty bits');
assert(visualFrame(4) === visualBaseline - 1, 'combined hidden position/material batch');
part.visible(true); assert(visualFrame(5) === visualBaseline, 'recover show after combined batch');
const visualPose = new Float32Array(9); visual.instance.readPose(1,visualPose);
assert(equal(visualPose.subarray(0,3),new Float32Array([2,3,4])), 'combined position reached native');
const visualStable = snapshotHash();
rejects(() => part.place({position:[99,88,77],scale:[1,0,1]}));
rejects(() => part.material({color:0xff0000,opacity:2}));
assert(visualFrame(6) === visualBaseline && snapshotHash() === visualStable, 'invalid wrapper patches preserve native frame');
forceSubmitFailure(2); rejects(() => visual.render());
assert(snapshotHash() === visualStable, 'wrapper submit failure preserves previous recorder packets');
forceSubmitFailure(0); clearPackets(); visual.render(); assert(snapshotHash() === visualStable, 'wrapper submit recovery');
part.visible(false); assert(visualFrame(7) === visualBaseline - 1, 'wrapper hide remains usable after failure');
part.visible(true); assert(visualFrame(8) === visualBaseline, 'wrapper final show recovery');
visual.dispose(); visualModel.dispose();
