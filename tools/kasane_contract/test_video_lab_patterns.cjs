const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../../apps/kasane/video_lab.js'), 'utf8');
const frames = [];
const selected = [];
const labels = [];
const logs = [];
let onAction;
let resource;
const title = {setText: (_, value) => labels.push(value)};
const tx = {
  background: () => {},
  text: options => {
    if (options.text === '1/4 SWEEP') assert(options.capacity >= '4/4 INTERFERENCE'.length);
    return title;
  },
  image: options => {resource = options.resource;},
};
const context = {
  pocket: {
    input: {onAction: callback => {onAction = callback;}},
    kasane: {
      video: {
        open: (width, height) => {
          assert.deepEqual([width, height], [64, 48]);
          return 9;
        },
        push: (pixels, timestamp) => {
          frames.push({pixels: Array.from(pixels), timestamp});
          return true;
        },
        select: timestamp => {selected.push(timestamp); return true;},
      },
      replace: callback => callback(tx),
      patch: callback => callback(tx),
    },
  },
  console: {log: message => logs.push(message)},
};
vm.runInNewContext(source, context, {filename: 'video_lab.js'});
assert.equal(resource, 9);
assert.equal(typeof onAction, 'function');

function frame() {
  context.frame();
  return frames.at(-1).pixels;
}
function action(direction, phase = 'press') {
  onAction({action: direction, phase});
}
function signature(pixels) {
  let hash = 2166136261;
  for (const pixel of pixels) hash = Math.imul(hash ^ pixel, 16777619) >>> 0;
  return hash;
}

const signatures = [signature(frame())];
action('right');
assert.equal(labels.at(-1), '2/4 COLOR BARS');
const bars = frame();
assert.equal(bars[0], 0xffff);
assert.equal(bars[8], 0xffe0);
assert.equal(bars[128 + 8], 0xffe0);
assert.equal(bars[64], 0xffff); // animated white scan line
signatures.push(signature(bars));
action('down');
assert.equal(labels.at(-1), '3/4 CHECKER');
signatures.push(signature(frame()));
action('right');
assert.equal(labels.at(-1), '4/4 INTERFERENCE');
signatures.push(signature(frame()));
assert.equal(new Set(signatures).size, 4, 'patterns must draw different images');

action('right');
assert.equal(labels.at(-1), '1/4 SWEEP', 'forward direction wraps');
action('left');
assert.equal(labels.at(-1), '4/4 INTERFERENCE', 'left goes backward');
action('up');
assert.equal(labels.at(-1), '3/4 CHECKER', 'up goes backward');
action('down', 'repeat');
assert.equal(labels.at(-1), '4/4 INTERFERENCE', 'held arrow repeats');
const labelCount = labels.length;
action('left', 'release');
action('accept');
assert.equal(labels.length, labelCount, 'release and other actions do not select');
assert.deepEqual(frames.map(item => item.timestamp), [0, 33333, 66666, 99999]);
assert.deepEqual(selected, frames.map(item => item.timestamp));
assert.equal(frames.every(item => item.pixels.length === 64 * 48), true);
assert.equal(logs.filter(line => line.startsWith('VIDEO_LAB PATTERN ')).length, 7);
console.log('VIDEO LAB pattern selection and frame output PASS');
