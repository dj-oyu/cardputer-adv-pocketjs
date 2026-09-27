// Host-side scene contract for the procedural image news demo.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = path.resolve(__dirname, '../../apps/kasane/proc_news_zoom.js');
const monitor = [64, 20, 176, 83];
const full = [0, 0, 240, 135];
const records = {resources: 0, programs: [], frames: [], images: [], patches: [], events: []};
const resource = {kind: 'procedural'};
let pendingFrame;

const tx = {
  background() {}, gradient() {}, rect() {}, text() {},
  image(spec) {
    const image = {resource: spec.resource, bounds: Array.from(spec.bounds),
      initialBounds: Array.from(spec.bounds),
      clip: spec.clip && Array.from(spec.clip), sourceWidth: spec.sourceWidth,
      sourceHeight: spec.sourceHeight};
    records.images.push(image);
    return {
      setRect(patchTx, bounds) {
        assert.equal(patchTx, tx);
        image.bounds = Array.from(bounds);
        records.patches.push({bounds: image.bounds, clip: image.clip.slice()});
      }
    };
  }
};
const host = {
  resource() { ++records.resources; return resource; },
  register(program, batch) {
    records.programs.push({program, batch});
    return records.programs.length - 1;
  },
  beginFrame(backdrop) { pendingFrame = {backdrop, draws: []}; },
  draw(handle, inputs) { pendingFrame.draws.push({handle, inputs}); },
  commit() {
    records.events.push('commit');
    records.frames.push(pendingFrame);
    pendingFrame = null;
  }
};
const view = {
  procedural: host,
  replace(build) { records.events.push('replace'); build(tx); },
  patch(update) { records.events.push('patch'); update(tx); }
};
const context = {pocket: {kasane: view}};
vm.runInNewContext(fs.readFileSync(source, 'utf8'), context, {filename: source});
for (let tick = 0; tick < 320; ++tick) context.frame();

assert.equal(records.resources, 1);
assert.equal(records.programs.length, 15);
assert.equal(records.programs.filter(item => item.batch).length, 3);
assert.equal(records.images.length, 1);
assert.equal(records.images[0].resource, resource);
assert.equal(records.images[0].sourceWidth, 240);
assert.equal(records.images[0].sourceHeight, 135);
assert.deepEqual(records.events.slice(0, 4),
  ['commit', 'replace', 'commit', 'patch']);
assert.deepEqual(records.images[0].initialBounds, monitor);
assert.equal(records.patches.length, 319);
assert.deepEqual(records.patches[0].bounds, monitor);
assert.deepEqual(records.patches[63].bounds, full);
assert.deepEqual(records.patches[159].bounds, monitor);
assert.deepEqual(records.patches[223].bounds, full);

// setRect must not leave the original monitor-sized clip on the moving image.
// The full-panel clip is fixed throughout the inset, both zooms and full view.
assert.deepEqual(records.images[0].clip, full);
for (const patch of records.patches) assert.deepEqual(patch.clip, full);
assert.notDeepEqual(records.patches[0].clip, monitor);

const inset = records.patches[0].bounds;
const zoomIn = records.patches[42].bounds;
const expanded = records.patches[90].bounds;
const zoomOut = records.patches[138].bounds;
assert.notDeepEqual(zoomIn, inset);
assert.notDeepEqual(zoomIn, expanded);
assert.notDeepEqual(zoomOut, inset);
assert.notDeepEqual(zoomOut, expanded);
assert(zoomIn[2] - zoomIn[0] > inset[2] - inset[0]);
assert(zoomOut[2] - zoomOut[0] < expanded[2] - expanded[0]);

assert.equal(records.frames.length, 320);
assert(records.frames.every(frame => frame.draws.length === 5));
assert.notEqual(records.frames[15].backdrop, records.frames[16].backdrop);
assert.notEqual(records.frames[31].backdrop, records.frames[32].backdrop);
assert.equal(records.frames[0].backdrop, records.frames[48].backdrop);
assert.notEqual(records.frames[159].backdrop, records.frames[160].backdrop);
console.log('proc_news_zoom scene PASS: fixed full clip and animated bounds');
