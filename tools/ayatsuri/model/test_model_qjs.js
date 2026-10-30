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
rejects(() => modelOpen(new ArrayBuffer(4)), 'INVALID_ARGUMENT');
if (typeof SharedArrayBuffer === 'function') rejects(() => modelOpen(new SharedArrayBuffer(64)), 'INVALID_ARGUMENT');
const detached = new ArrayBuffer(8);
if (typeof detached.transfer === 'function') { detached.transfer(); rejects(() => modelOpen(detached)); }
rejects(() => { model.count = 0; });
assert(model.node(firstName) === 0, 'stable named node');
rejects(() => model.node({toString() { throw Error('must not coerce'); }}), 'INVALID_ARGUMENT');
const a = model.instance(), b = model.instance(), before = new Float32Array(9), other = new Float32Array(9);
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
rejects(() => model.instance(), 'CLOSED');
rejects(() => model.node(firstName), 'CLOSED');
// Closing Model prohibits new owners; its two existing actors remain usable.
a.evaluate(camera, light); b.evaluate(camera, light);
clearPackets(); b.submit();
a.dispose(); rejects(() => a.evaluate(camera, light), 'CLOSED');
rejects(() => a.stats({}), 'CLOSED');
b.dispose();
// A user expando may close a cycle across the native retained Model value.
let cycleModel = staticModel(), cycleActor = cycleModel.instance();
cycleModel.actor = cycleActor; cycleActor = null; cycleModel = null;
assert(typeof staticModel === 'function', 'static path exists');
