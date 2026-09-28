const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const lab = fs.readFileSync('apps/kasane/video_stream_lab.js', 'utf8');
const recover = fs.readFileSync('apps/kasane/video_stream_recover.js', 'utf8');
const tick = () => new Promise(resolve => setImmediate(resolve));

async function run() {
  const writes = [];
  let sleeps = 0, streamStarted = false;
  const diagnostic = {
    kasane: {
      video: {open: () => 1, streamStart: () => { streamStarted = true; }},
      replace: () => {},
    },
    fs: {
      requestFolder: async () => 'sd:/',
      stat: async () => {throw {code: 'NOT_FOUND'};},
      open: async (_, options) => {
        assert.equal(options.mode, 'create');
        assert.equal(options.durability, 'synced');
        return {
          write: async bytes => {writes.push(Buffer.from(bytes)); return bytes.length;},
          commit: async () => ({}),
          close: () => {},
        };
      },
    },
    time: {sleep: async () => {++sleeps; await tick();}, now: () => 1},
  };
  const context = {pocket: diagnostic, console: {log: () => {}}};
  vm.runInNewContext(lab, context);
  context.frame();
  for (let i = 0; i < 200 && !streamStarted; ++i) await tick();
  assert(streamStarted, 'diagnostic did not finish its writes');
  assert.equal(writes.length, 61);
  assert.equal(sleeps, 61);
  assert(writes.every(bytes => bytes.length <= 524));
  const data = Buffer.concat(writes);
  assert.equal(data.length, 16 + 60 * 524);
  assert.equal(data.subarray(0, 4).toString(), 'KSV1');

  async function recovery(bytes) {
    let at = 0, removed = 0, closed = 0;
    const logs = [];
    const pocket = {
      fs: {
        requestFolder: async () => 'sd:/',
        stat: async () => ({sizeBytes: bytes.length}),
        open: async () => ({
          read: async max => {
            if (at === bytes.length) return null;
            const part = bytes.subarray(at, at + Math.min(max, 257));
            at += part.length;
            return part;
          },
          close: () => {++closed;},
        }),
        remove: async () => {++removed;},
      },
      time: {sleep: tick},
    };
    await vm.runInNewContext(recover, {pocket, console: {log: value => logs.push(value)}});
    return {removed, closed, logs};
  }
  const matched = await recovery(data);
  assert.equal(matched.removed, 1);
  assert.equal(matched.closed, 1);
  const altered = Buffer.from(data);
  altered[100] ^= 1;
  const refused = await recovery(altered);
  assert.equal(refused.removed, 0);
  assert(refused.logs.some(line => line.includes('RECOVER_REFUSED')));
  console.log('D5 diagnostic streamed writes and exact recovery PASS');
}
run().catch(error => {console.error(error); process.exitCode = 1;});
